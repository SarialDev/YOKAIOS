#include "drivers/tty.h"
#include "hal/input.h"
#include "hal/console.h"
#include "keycodes.h"
#include "lib/kprintf.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"
#include "uapi/syscall_nr.h"
#include "proc/signal.h"

namespace {

const uint32_t RX_SIZE = 1024;          // putere a lui 2: octeti gata de citit
const uint32_t LINE_MAX = 256;          // linia in lucru (modul canonic)

Spinlock lock("tty");
WaitQueue readq;
uint8_t rx[RX_SIZE];
uint32_t rx_head, rx_tail;
uint8_t line[LINE_MAX];
uint32_t line_len;
bool eof_pending;
bool is_active;
uint64_t fg_pgid;                       // grupul din prim-plan: primeste SIGINT / SIGQUIT de la tastatura
int pending_sig;                        // semnal de trimis dupa ce se elibereaza lock-ul tty
uint64_t rx_total;
KTermios term = {0x100 /* ICRNL */, 0x5 /* OPOST|ONLCR */, 0xBF /* CS8|CREAD|... */, TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE,
                 0, {3, 0x1C, 0x7F, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1A, 0, 0x12, 0xF, 0x17, 0x16, 0, 0, 0}};

bool rx_empty(){
    return rx_head == rx_tail;
}

void rx_push(uint8_t c){
    if(rx_head - rx_tail < RX_SIZE){
        rx[rx_head++ & (RX_SIZE - 1)] = c;
    }
}

// Ecou: direct pe console, fara sa treaca prin tty_write (care nu ia lock-ul tty)
void echo(const char* s, uint32_t n){
    if(term.c_lflag & TTY_ECHO){
        kwrite(s, n);
    }
}

void commit_line(bool with_newline_already){
    (void)with_newline_already;
    for(uint32_t i = 0; i < line_len; i++){
        rx_push(line[i]);
    }
    line_len = 0;
}

// Un octet primit, cu lock-ul tty luat
void input_byte(uint8_t c){
    // ISIG: Ctrl-C / Ctrl-\\ nu ajung la program, ci devin SIGINT / SIGQUIT pentru grupul din prim-plan
    if((term.c_lflag & TTY_ISIG) && (c == term.c_cc[0] || c == term.c_cc[1]) && c){
        echo(c == term.c_cc[0] ? "^C\n" : "^\\\n", 3);
        line_len = 0;
        pending_sig = c == term.c_cc[0] ? SIGINT : SIGQUIT;
        return;
    }
    if(!(term.c_lflag & TTY_ICANON)){
        rx_push(c);
        if(c >= 0x20 && c < 0x7F){
            echo((const char*)&c, 1);
        } else if(c == '\n'){
            echo("\n", 1);
        }
        return;
    }
    switch(c){
    case '\n':
        if(line_len < LINE_MAX){
            line[line_len++] = '\n';
        }
        echo("\n", 1);
        commit_line(true);
        break;
    case 0x7F:
    case '\b':
        if(line_len > 0){
            line_len--;
            echo("\b \b", 3);
        }
        break;
    case 0x04:                  // Ctrl-D: trimite linia asa cum e; pe linie goala = EOF
        if(line_len == 0){
            eof_pending = true;
        } else {
            commit_line(false);
        }
        break;
    case 0x15:                  // Ctrl-U
        while(line_len > 0){
            line_len--;
            echo("\b \b", 3);
        }
        break;
    case 0x03:                  // Ctrl-C fara ISIG: renunta la linie
        echo("^C\n", 3);
        line_len = 0;
        break;
    default:
        if((c >= 0x20 && c < 0x7F) || c == '\t'){
            if(line_len < LINE_MAX - 1){
                line[line_len++] = c;
                echo((const char*)&c, 1);
            }
        }
        break;
    }
}

// Tasta -> octeti (secvente ESC [ ... ca pe terminalele ANSI); numarul de octeti, 0 daca tasta nu produce nimic
uint32_t key_bytes(const KeyEvent& k, char* out){
    switch(k.keycode){
    case KEY_ENTER:     out[0] = '\n'; return 1;
    case KEY_BACKSPACE: out[0] = 0x7F; return 1;
    case KEY_TAB:       out[0] = '\t'; return 1;
    case KEY_ESC:       out[0] = 0x1B; return 1;
    case KEY_UP:        __builtin_memcpy(out, "\x1b[A", 3); return 3;
    case KEY_DOWN:      __builtin_memcpy(out, "\x1b[B", 3); return 3;
    case KEY_RIGHT:     __builtin_memcpy(out, "\x1b[C", 3); return 3;
    case KEY_LEFT:      __builtin_memcpy(out, "\x1b[D", 3); return 3;
    case KEY_HOME:      __builtin_memcpy(out, "\x1b[H", 3); return 3;
    case KEY_END:       __builtin_memcpy(out, "\x1b[F", 3); return 3;
    case KEY_INSERT:    __builtin_memcpy(out, "\x1b[2~", 4); return 4;
    case KEY_DELETE:    __builtin_memcpy(out, "\x1b[3~", 4); return 4;
    case KEY_PGUP:      __builtin_memcpy(out, "\x1b[5~", 4); return 4;
    case KEY_PGDN:      __builtin_memcpy(out, "\x1b[6~", 4); return 4;
    default:
        break;
    }
    char a = k.ascii;
    if(!a){
        return 0;
    }
    if(k.modifiers & MOD_CTRL){
        if(a >= 'a' && a <= 'z'){
            a = (char)(a - 'a' + 1);
        } else if(a >= 'A' && a <= 'Z'){
            a = (char)(a - 'A' + 1);
        } else {
            return 0;
        }
    }
    out[0] = a;
    return 1;
}

// Apelat din IRQ (tastatura PS/2 sau USB) pentru fiecare eveniment de pe event bus
void on_input(const InputEvent& ev){
    if(ev.type != InputType::Key || !ev.key.pressed){
        return;
    }
    char bytes[4];
    uint32_t n = key_bytes(ev.key, bytes);
    if(!n){
        return;
    }
    uint64_t flags = lock.lock_irqsave();
    rx_total += n;
    if(!(term.c_lflag & TTY_ICANON) || n == 1){
        for(uint32_t i = 0; i < n; i++){
            input_byte((uint8_t)bytes[i]);
        }
    }                                   // in modul canonic secventele de sageti se ignora
    bool ready = !rx_empty() || eof_pending;
    int sig = pending_sig;
    pending_sig = 0;
    uint64_t pgid = fg_pgid;
    lock.unlock_irqrestore(flags);
    if(ready){
        readq.wake_all();
    }
    if(sig && pgid){
        signal_send_pgrp(pgid, sig, false);
    }
}

}

void tty_init(){
    input_subscribe(on_input);
}

void tty_activate(){
    uint64_t flags = lock.lock_irqsave();
    is_active = true;
    lock.unlock_irqrestore(flags);
}

bool tty_active(){
    return is_active;
}

int64_t tty_read(void* buf, uint64_t len, bool nonblock){
    if(!is_active){
        return 0;
    }
    uint8_t* o = (uint8_t*)buf;
    uint64_t flags = lock.lock_irqsave();
    while(rx_empty()){
        if(eof_pending){
            eof_pending = false;
            lock.unlock_irqrestore(flags);
            return 0;
        }
        if(nonblock){
            lock.unlock_irqrestore(flags);
            return -EAGAIN;
        }
        if(!readq.wait_intr(lock) && rx_empty()){
            lock.unlock_irqrestore(flags);
            return -EINTR;
        }
    }
    bool canon = term.c_lflag & TTY_ICANON;
    uint64_t n = 0;
    while(n < len && !rx_empty()){
        uint8_t c = rx[rx_tail++ & (RX_SIZE - 1)];
        o[n++] = c;
        if(canon && c == '\n'){
            break;
        }
    }
    lock.unlock_irqrestore(flags);
    return (int64_t)n;
}

int64_t tty_write(const void* buf, uint64_t len){
    kwrite((const char*)buf, len);
    return (int64_t)len;
}

void tty_get_termios(KTermios& t){
    uint64_t flags = lock.lock_irqsave();
    t = term;
    lock.unlock_irqrestore(flags);
}

void tty_set_termios(const KTermios& t){
    uint64_t flags = lock.lock_irqsave();
    bool canon_changed = (term.c_lflag ^ t.c_lflag) & TTY_ICANON;
    term = t;
    if(canon_changed){
        line_len = 0;                   // linia neterminata se pierde la schimbarea modului
    }
    lock.unlock_irqrestore(flags);
}

void tty_get_winsize(KWinsize& w){
    Console* c = hal_console_screen();
    w.ws_col = (uint16_t)(c && c->cols() ? c->cols() : 80);
    w.ws_row = (uint16_t)(c && c->rows() ? c->rows() : 25);
    w.ws_xpixel = w.ws_ypixel = 0;
}

uint64_t tty_rx_count(){
    return rx_total;
}

void tty_set_pgrp(uint64_t pgid){
    __atomic_store_n(&fg_pgid, pgid, __ATOMIC_RELEASE);
}

uint64_t tty_get_pgrp(){
    return __atomic_load_n(&fg_pgid, __ATOMIC_ACQUIRE);
}

void tty_inject(const char* s, uint64_t n){
    uint64_t flags = lock.lock_irqsave();
    for(uint64_t i = 0; i < n; i++){
        input_byte((uint8_t)s[i]);
    }
    bool ready = !rx_empty() || eof_pending;
    int sig = pending_sig;
    pending_sig = 0;
    uint64_t pgid = fg_pgid;
    lock.unlock_irqrestore(flags);
    if(ready){
        readq.wake_all();
    }
    if(sig && pgid){
        signal_send_pgrp(pgid, sig, false);
    }
}
