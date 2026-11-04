#include "drivers/pty.h"
#include "sync/pollwait.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"
#include "proc/signal.h"
#include "uapi/syscall_nr.h"
#include "uapi/net.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "lib/string.h"
#include "syscall/syscall.h"

namespace {

const uint32_t RX_SIZE = 1024;          // master -> slave (dupa disciplina de linie); putere a lui 2
const uint32_t TX_SIZE = 4096;          // slave -> master (+ ecoul); putere a lui 2
const uint32_t LINE_MAX = 256;

const uint32_t IFLAG_ICRNL = 0x100;
const uint32_t OFLAG_ONLCR = 0x4;
const int VMIN = 6;

struct Pty {
    Spinlock lock{"pty"};
    WaitQueue mq;                       // cititori de pe master (asteapta iesire de la slave)
    WaitQueue sq;                       // cititori de pe slave (asteapta intrare)
    WaitQueue mw;                       // scriitori pe master (rx plin)
    WaitQueue sw;                       // scriitori pe slave (tx plin)
    bool master_open = true;
    bool slave_open = true;
    KTermios term = {0x100 /* ICRNL */, 0x5 /* OPOST|ONLCR */, 0xBF, TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE,
                     0, {3, 0x1C, 0x7F, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1A, 0, 0x12, 0xF, 0x17, 0x16, 0, 0, 0}};
    KWinsize ws = {24, 80, 0, 0};
    uint64_t pgid = 0;
    uint8_t rx[RX_SIZE];
    uint32_t rx_head = 0, rx_tail = 0;
    uint8_t tx[TX_SIZE];
    uint32_t tx_head = 0, tx_tail = 0;
    uint8_t line[LINE_MAX];
    uint32_t line_len = 0;
    bool eof_pending = false;
    int pending_sig = 0;
    int esc_state = 0;                  // 1 = dupa ESC, 2 = in secventa CSI (modul canonic le ignora)
};

Spinlock table_lock("pty-table");
int npty;

bool rx_empty(const Pty* p){ return p->rx_head == p->rx_tail; }
uint32_t rx_free(const Pty* p){ return RX_SIZE - (p->rx_head - p->rx_tail); }
bool tx_empty(const Pty* p){ return p->tx_head == p->tx_tail; }
uint32_t tx_free(const Pty* p){ return TX_SIZE - (p->tx_head - p->tx_tail); }

void rx_push(Pty* p, uint8_t c){
    if(rx_free(p)){
        p->rx[p->rx_head++ & (RX_SIZE - 1)] = c;
    }
}

void tx_push(Pty* p, uint8_t c){
    if(tx_free(p)){
        p->tx[p->tx_head++ & (TX_SIZE - 1)] = c;
    }
}

// Ecou catre master (lock luat); iesirea ONLCR se aplica si ecoului
void echo(Pty* p, const char* s, uint32_t n){
    if(!(p->term.c_lflag & TTY_ECHO)){
        return;
    }
    for(uint32_t i = 0; i < n; i++){
        if(s[i] == '\n' && (p->term.c_oflag & OFLAG_ONLCR)){
            tx_push(p, '\r');
        }
        tx_push(p, (uint8_t)s[i]);
    }
}

void commit_line(Pty* p){
    for(uint32_t i = 0; i < p->line_len; i++){
        rx_push(p, p->line[i]);
    }
    p->line_len = 0;
}

// Un octet venit de la master, cu lock-ul luat (aceeasi disciplina ca tty.cpp)
void input_byte(Pty* p, uint8_t c){
    const KTermios& t = p->term;
    if(c == '\r' && (t.c_iflag & IFLAG_ICRNL)){
        c = '\n';
    }
    if((t.c_lflag & TTY_ISIG) && c && (c == t.c_cc[0] || c == t.c_cc[1])){
        echo(p, c == t.c_cc[0] ? "^C\n" : "^\\\n", c == t.c_cc[0] ? 3 : 3);
        p->line_len = 0;
        p->pending_sig = c == t.c_cc[0] ? SIGINT : SIGQUIT;
        return;
    }
    if(!(t.c_lflag & TTY_ICANON)){
        rx_push(p, c);
        if(c >= 0x20 && c < 0x7F){
            echo(p, (const char*)&c, 1);
        } else if(c == '\n'){
            echo(p, "\n", 1);
        }
        return;
    }
    // modul canonic: secventele ESC [ ... se ignora
    if(p->esc_state == 0 && c == 0x1B){
        p->esc_state = 1;
        return;
    }
    if(p->esc_state == 1){
        p->esc_state = c == '[' || c == 'O' ? 2 : 0;
        return;
    }
    if(p->esc_state == 2){
        if(c >= 0x40 && c <= 0x7E){
            p->esc_state = 0;
        }
        return;
    }
    switch(c){
    case '\n':
        if(p->line_len < LINE_MAX){
            p->line[p->line_len++] = '\n';
        }
        echo(p, "\n", 1);
        commit_line(p);
        break;
    case 0x7F:
    case '\b':
        if(p->line_len > 0){
            p->line_len--;
            echo(p, "\b \b", 3);
        }
        break;
    case 0x04:
        if(p->line_len == 0){
            p->eof_pending = true;
        } else {
            commit_line(p);
        }
        break;
    case 0x15:
        while(p->line_len > 0){
            p->line_len--;
            echo(p, "\b \b", 3);
        }
        break;
    default:
        if((c >= 0x20 && c < 0x7F) || c == '\t' || c >= 0x80){
            if(p->line_len < LINE_MAX - 1){
                p->line[p->line_len++] = c;
                echo(p, (const char*)&c, 1);
            }
        }
        break;
    }
}

void send_pending(uint64_t pgid, int sig){
    if(sig && pgid){
        signal_send_pgrp(pgid, sig, false);
    }
}

// --- slave ---

int64_t slave_read(File* f, void* buf, uint64_t len, bool nonblock){
    Pty* p = (Pty*)f->priv;
    uint8_t* o = (uint8_t*)buf;
    uint64_t flags = p->lock.lock_irqsave();
    while(rx_empty(p)){
        if(p->eof_pending){
            p->eof_pending = false;
            p->lock.unlock_irqrestore(flags);
            return 0;
        }
        if(!p->master_open){
            p->lock.unlock_irqrestore(flags);
            return 0;
        }
        if(!(p->term.c_lflag & TTY_ICANON) && p->term.c_cc[VMIN] == 0){
            p->lock.unlock_irqrestore(flags);
            return 0;                   // VMIN = 0: fara asteptare
        }
        if(nonblock){
            p->lock.unlock_irqrestore(flags);
            return -EAGAIN;
        }
        if(!p->sq.wait_intr(p->lock) && rx_empty(p)){
            p->lock.unlock_irqrestore(flags);
            return -EINTR;
        }
    }
    bool canon = p->term.c_lflag & TTY_ICANON;
    uint64_t n = 0;
    while(n < len && !rx_empty(p)){
        uint8_t c = p->rx[p->rx_tail++ & (RX_SIZE - 1)];
        o[n++] = c;
        if(canon && c == '\n'){
            break;
        }
    }
    p->mw.wake_all();
    poll_notify();
    p->lock.unlock_irqrestore(flags);
    return (int64_t)n;
}

int64_t slave_write(File* f, const void* src, uint64_t len, bool nonblock){
    Pty* p = (Pty*)f->priv;
    const uint8_t* s = (const uint8_t*)src;
    uint64_t done = 0;
    uint64_t flags = p->lock.lock_irqsave();
    while(done < len){
        if(!p->master_open){
            p->lock.unlock_irqrestore(flags);
            return done ? (int64_t)done : -EIO;
        }
        if(tx_free(p) < 2){
            p->mq.wake_all();
            poll_notify();
            if(nonblock){
                p->lock.unlock_irqrestore(flags);
                return done ? (int64_t)done : -EAGAIN;
            }
            if(!p->sw.wait_intr(p->lock) && tx_free(p) < 2){
                p->lock.unlock_irqrestore(flags);
                return done ? (int64_t)done : -EINTR;
            }
            continue;
        }
        uint8_t c = s[done++];
        if(c == '\n' && (p->term.c_oflag & OFLAG_ONLCR)){
            tx_push(p, '\r');
        }
        tx_push(p, c);
    }
    p->mq.wake_all();
    poll_notify();
    p->lock.unlock_irqrestore(flags);
    return (int64_t)done;
}

uint32_t slave_poll(File* f){
    Pty* p = (Pty*)f->priv;
    uint64_t flags = p->lock.lock_irqsave();
    uint32_t r = POLLOUT;
    if(!rx_empty(p) || p->eof_pending){
        r |= POLLIN;
    }
    if(!p->master_open){
        r |= POLLIN | POLLHUP;
    }
    p->lock.unlock_irqrestore(flags);
    return r;
}

// --- master ---

int64_t master_read(File* f, void* buf, uint64_t len, bool nonblock){
    Pty* p = (Pty*)f->priv;
    uint8_t* o = (uint8_t*)buf;
    uint64_t flags = p->lock.lock_irqsave();
    while(tx_empty(p)){
        if(!p->slave_open){
            p->lock.unlock_irqrestore(flags);
            return 0;
        }
        if(nonblock){
            p->lock.unlock_irqrestore(flags);
            return -EAGAIN;
        }
        if(!p->mq.wait_intr(p->lock) && tx_empty(p)){
            p->lock.unlock_irqrestore(flags);
            return -EINTR;
        }
    }
    uint64_t n = 0;
    while(n < len && !tx_empty(p)){
        o[n++] = p->tx[p->tx_tail++ & (TX_SIZE - 1)];
    }
    p->sw.wake_all();
    poll_notify();
    p->lock.unlock_irqrestore(flags);
    return (int64_t)n;
}

int64_t master_write(File* f, const void* src, uint64_t len, bool nonblock){
    Pty* p = (Pty*)f->priv;
    const uint8_t* s = (const uint8_t*)src;
    uint64_t done = 0;
    uint64_t flags = p->lock.lock_irqsave();
    int sig = 0;
    uint64_t pgid = 0;
    while(done < len){
        if(!p->slave_open){
            break;
        }
        if(rx_free(p) < LINE_MAX + 2){
            if(nonblock){
                break;
            }
            if(!p->mw.wait_intr(p->lock) && rx_free(p) < LINE_MAX + 2){
                break;
            }
            continue;
        }
        input_byte(p, s[done++]);
        if(p->pending_sig){
            sig = p->pending_sig;
            pgid = p->pgid;
            p->pending_sig = 0;
        }
    }
    p->sq.wake_all();
    p->mq.wake_all();                   // ecoul
    poll_notify();
    p->lock.unlock_irqrestore(flags);
    send_pending(pgid, sig);
    if(!done && len){
        return !p->slave_open ? -EIO : -EAGAIN;
    }
    return (int64_t)done;
}

uint32_t master_poll(File* f){
    Pty* p = (Pty*)f->priv;
    uint64_t flags = p->lock.lock_irqsave();
    uint32_t r = 0;
    if(!tx_empty(p)){
        r |= POLLIN;
    }
    if(!p->slave_open){
        r |= POLLIN | POLLHUP;
    } else if(rx_free(p) >= LINE_MAX + 2){
        r |= POLLOUT;
    }
    p->lock.unlock_irqrestore(flags);
    return r;
}

void release(Pty* p){
    p->~Pty();
    kfree(p);
    uint64_t fl = table_lock.lock_irqsave();
    npty--;
    table_lock.unlock_irqrestore(fl);
}

void master_close(File* f){
    Pty* p = (Pty*)f->priv;
    uint64_t flags = p->lock.lock_irqsave();
    p->master_open = false;
    uint64_t pgid = p->pgid;
    bool last = !p->slave_open;
    p->sq.wake_all();
    p->sw.wake_all();
    poll_notify();
    p->lock.unlock_irqrestore(flags);
    if(!last){
        send_pending(pgid, SIGHUP);
    } else {
        release(p);
    }
}

void slave_close(File* f){
    Pty* p = (Pty*)f->priv;
    uint64_t flags = p->lock.lock_irqsave();
    p->slave_open = false;
    bool last = !p->master_open;
    p->mq.wake_all();
    p->mw.wake_all();
    poll_notify();
    p->lock.unlock_irqrestore(flags);
    if(last){
        release(p);
    }
}

const FileOps master_ops = {master_read, master_write, master_close, nullptr, nullptr, master_poll};
const FileOps slave_ops = {slave_read, slave_write, slave_close, nullptr, nullptr, slave_poll};

}

int pty_create(File*& master, File*& slave, const KWinsize* ws){
    uint64_t fl = table_lock.lock_irqsave();
    if(npty >= PTY_MAX){
        table_lock.unlock_irqrestore(fl);
        return -ENOSPC;
    }
    npty++;
    table_lock.unlock_irqrestore(fl);
    void* mem = kmalloc(sizeof(Pty));
    Pty* p = mem ? new (mem) Pty() : nullptr;
    File* m = p ? file_alloc(&master_ops, p) : nullptr;
    File* s = m ? file_alloc(&slave_ops, p) : nullptr;
    if(!s){
        if(m){
            kfree(m);
        }
        if(p){
            p->~Pty();
        }
        if(mem){
            kfree(mem);
        }
        fl = table_lock.lock_irqsave();
        npty--;
        table_lock.unlock_irqrestore(fl);
        return -ENOMEM;
    }
    if(ws && ws->ws_row && ws->ws_col){
        p->ws = *ws;
    }
    master = m;
    slave = s;
    return 0;
}

bool pty_is(const File* f){
    return f && (f->ops == &master_ops || f->ops == &slave_ops);
}

int pty_count(){
    return __atomic_load_n(&npty, __ATOMIC_ACQUIRE);
}

int64_t pty_ioctl(File* f, uint64_t cmd, uint64_t arg){
    Pty* p = (Pty*)f->priv;
    switch(cmd){
    case TCGETS: {
        KTermios t;
        uint64_t fl = p->lock.lock_irqsave();
        t = p->term;
        p->lock.unlock_irqrestore(fl);
        return copy_to_user(arg, &t, sizeof(t)) ? 0 : -EFAULT;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        KTermios t;
        if(!copy_from_user(&t, arg, sizeof(t))){
            return -EFAULT;
        }
        uint64_t fl = p->lock.lock_irqsave();
        bool canon_changed = (p->term.c_lflag ^ t.c_lflag) & TTY_ICANON;
        p->term = t;
        if(canon_changed){
            p->line_len = 0;
            p->esc_state = 0;
        }
        p->sq.wake_all();
        poll_notify();
        p->lock.unlock_irqrestore(fl);
        return 0;
    }
    case TIOCGPGRP: {
        int32_t pg = (int32_t)__atomic_load_n(&p->pgid, __ATOMIC_ACQUIRE);
        return copy_to_user(arg, &pg, sizeof(pg)) ? 0 : -EFAULT;
    }
    case TIOCSPGRP: {
        int32_t pg;
        if(!copy_from_user(&pg, arg, sizeof(pg))){
            return -EFAULT;
        }
        if(pg <= 0){
            return -EINVAL;
        }
        __atomic_store_n(&p->pgid, (uint64_t)pg, __ATOMIC_RELEASE);
        return 0;
    }
    case TIOCGWINSZ: {
        KWinsize w;
        uint64_t fl = p->lock.lock_irqsave();
        w = p->ws;
        p->lock.unlock_irqrestore(fl);
        return copy_to_user(arg, &w, sizeof(w)) ? 0 : -EFAULT;
    }
    case TIOCSWINSZ: {
        KWinsize w;
        if(!copy_from_user(&w, arg, sizeof(w))){
            return -EFAULT;
        }
        uint64_t fl = p->lock.lock_irqsave();
        bool changed = w.ws_row != p->ws.ws_row || w.ws_col != p->ws.ws_col;
        p->ws = w;
        uint64_t pgid = p->pgid;
        p->lock.unlock_irqrestore(fl);
        if(changed){
            send_pending(pgid, SIGWINCH);
        }
        return 0;
    }
    default:
        return -EINVAL;
    }
}
