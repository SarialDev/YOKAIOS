/* appstest (T10.5): pseudo-terminale din kernel (openpty) si aplicatiile GUI yterm / yfiles / ysettings in sesiunea pornita de init.
 * Rulat de test=apps; iese cu 0 daca totul trece (cu /etc/appshold asteapta 3 s la "appstest: shot <nume>" pentru capturi).
 *   A. PTY: ecou si linie canonica, ICRNL / ONLCR, mod brut + VMIN=0, ISIG (Ctrl-C -> SIGINT pe grupul din prim-plan), TIOCGWINSZ /
 *      TIOCSWINSZ, poll, EOF in ambele sensuri, ysh pe pty, limita de 8 pty-uri
 *   B. settimeofday (CAP_SYS_ADMIN)
 *   C. `init -g -1` porneste ycomp -p (compozitor + panou); din bara se pornesc Terminal, Fisiere si Setari
 *   D. Ctrl+Alt+Backspace opreste sesiunea, init iese cu 0 */
#include "ywin.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/yinput.h>

#define PANEL_H   40
#define PIN_X     8
#define PIN_W     38
#define PIN_GAP   2

#define K_A     30
#define K_BS    14
#define K_ENTER 28
#define K_ESC   1
#define K_F2    59
#define K_F3    60
#define K_F4    61
#define K_F5    62
#define K_HOME  72
#define K_RIGHT 76
#define K_LEFT  75
#define K_UP    73
#define K_DOWN  78
#define K_DEL   81

static int checks, fails;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)){ \
        fails++; \
        printf("appstest: FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

static long long ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void msleep(int n){
    struct timespec ts = {n / 1000, (long)(n % 1000) * 1000000L};
    while(nanosleep(&ts, &ts) < 0 && errno == EINTR){
    }
}

/* ================= A. pty ================= */

static int rd_to(int fd, char *buf, int n, int t){
    struct pollfd p = {fd, POLLIN, 0};
    if(poll(&p, 1, t) <= 0){
        return -1;
    }
    return (int)read(fd, buf, (size_t)n);
}

static int read_until(int fd, char *out, int cap, const char *want, int t){
    int len = 0;
    out[0] = 0;
    long long end = ms() + t;
    while(ms() < end && len < cap - 1){
        int n = rd_to(fd, out + len, cap - 1 - len, 50);
        if(n > 0){
            len += n;
            out[len] = 0;
            if(want && strstr(out, want)){
                return len;
            }
        } else if(n == 0){
            break;
        }
    }
    return len;
}

static int set_raw(int fd, int vmin){
    struct termios t;
    if(tcgetattr(fd, &t) < 0){
        return -1;
    }
    t.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = (cc_t)vmin;
    return tcsetattr(fd, TCSANOW, &t);
}

static void test_pty(void){
    int m, s;
    char buf[256];
    struct winsize ws = {30, 100, 0, 0};
    CHECK(openpty(&m, &s, NULL, NULL, &ws) == 0, "openpty: errno %d", errno);
    if(m < 0 || s < 0){
        return;
    }
    struct stat st;
    CHECK(fstat(s, &st) == 0 && S_ISCHR(st.st_mode), "slave e dispozitiv de caractere");
    CHECK(isatty(s) == 1, "isatty(slave)");
    struct winsize w2;
    CHECK(ioctl(s, TIOCGWINSZ, &w2) == 0 && w2.ws_row == 30 && w2.ws_col == 100, "dimensiunea initiala %dx%d", w2.ws_col, w2.ws_row);

    struct pollfd p = {s, POLLIN, 0};
    CHECK(poll(&p, 1, 0) == 0, "slave fara date: nu e citibil");
    CHECK(write(m, "abc", 3) == 3, "scriere pe master");
    CHECK(poll(&p, 1, 50) == 0, "linie neterminata: slave nu e citibil");
    write(m, "d\b\n", 3);
    p.revents = 0;
    CHECK(poll(&p, 1, 500) == 1 && (p.revents & POLLIN), "linia terminata face slave citibil");
    int n = (int)read(s, buf, sizeof(buf));
    CHECK(n == 4 && !memcmp(buf, "abc\n", 4), "slave citeste linia editata: %d", n);
    n = read_until(m, buf, sizeof(buf), "abc\r\n", 500);
    CHECK(strstr(buf, "abc") && strstr(buf, "\r\n"), "ecoul (%d octeti) ajunge pe master", n);
    write(m, "x\r", 2);
    n = (int)read(s, buf, sizeof(buf));
    CHECK(n == 2 && buf[0] == 'x' && buf[1] == '\n', "\\r -> \\n (%d)", n);
    read_until(m, buf, sizeof(buf), NULL, 100);
    write(s, "u\nv", 3);
    n = read_until(m, buf, sizeof(buf), "v", 500);
    CHECK(n == 4 && !memcmp(buf, "u\r\nv", 4), "ONLCR: %d octeti", n);
    p.fd = m;
    p.revents = 0;
    CHECK(poll(&p, 1, 0) == 0, "master fara date: nu e citibil");
    write(s, "z", 1);
    p.revents = 0;
    CHECK(poll(&p, 1, 500) == 1 && (p.revents & POLLIN), "master citibil dupa o scriere pe slave");
    read(m, buf, sizeof(buf));
    CHECK(set_raw(s, 1) == 0, "tcsetattr brut");
    write(m, "q", 1);
    n = rd_to(s, buf, sizeof(buf), 500);
    CHECK(n == 1 && buf[0] == 'q', "brut: un octet imediat (%d)", n);
    CHECK(set_raw(s, 0) == 0, "VMIN = 0");
    long long t0 = ms();
    n = (int)read(s, buf, sizeof(buf));
    CHECK(n == 0 && ms() - t0 < 500, "VMIN = 0: read fara date intoarce 0 imediat (%d)", n);
    write(m, "\x1b[A", 3);
    n = (int)read(s, buf, sizeof(buf));
    CHECK(n == 3 && !memcmp(buf, "\x1b[A", 3), "secventa ESC [ A trece intreaga in modul brut (%d)", n);
    struct winsize nw = {50, 132, 0, 0};
    CHECK(ioctl(m, TIOCSWINSZ, &nw) == 0, "TIOCSWINSZ");
    CHECK(ioctl(s, TIOCGWINSZ, &w2) == 0 && w2.ws_row == 50 && w2.ws_col == 132, "dimensiune noua %dx%d", w2.ws_col, w2.ws_row);
    struct termios t;
    tcgetattr(s, &t);
    t.c_lflag |= ICANON | ECHO | ISIG;
    t.c_cc[VMIN] = 1;
    tcsetattr(s, TCSANOW, &t);
    write(m, "\x04", 1);
    n = (int)read(s, buf, sizeof(buf));
    CHECK(n == 0, "Ctrl-D: EOF (%d)", n);
    close(s);
    n = rd_to(m, buf, sizeof(buf), 500);
    while(n > 0){
        n = rd_to(m, buf, sizeof(buf), 100);
    }
    CHECK(n == 0, "slave inchis: master citeste EOF (%d)", n);
    close(m);

    CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0, "al doilea openpty");
    CHECK(ioctl(s, TIOCGWINSZ, &w2) == 0 && w2.ws_row == 24 && w2.ws_col == 80, "implicit 80x24: %dx%d", w2.ws_col, w2.ws_row);
    close(m);
    n = (int)read(s, buf, sizeof(buf));
    CHECK(n == 0, "master inchis: slave citeste EOF (%d)", n);
    errno = 0;
    n = (int)write(s, "x", 1);
    CHECK(n < 0 && errno == EIO, "master inchis: scriere pe slave -> EIO (%d, errno %d)", n, errno);
    close(s);

    CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0, "openpty pentru ISIG");
    pid_t pid = fork();
    if(pid == 0){
        close(m);
        close(s);
        setpgid(0, 0);
        for(;;){
            sleep(1);
        }
    }
    setpgid(pid, pid);
    int pg = pid;
    CHECK(ioctl(s, TIOCSPGRP, &pg) == 0, "TIOCSPGRP");
    int got = -1;
    CHECK(ioctl(s, TIOCGPGRP, &got) == 0 && got == pid, "TIOCGPGRP: %d", got);
    msleep(100);
    write(m, "\x03", 1);
    int stt = 0;
    long long end = ms() + 5000;
    pid_t wr = 0;
    while(ms() < end && (wr = waitpid(pid, &stt, WNOHANG)) == 0){
        msleep(20);
    }
    CHECK(wr == pid && WIFSIGNALED(stt) && WTERMSIG(stt) == SIGINT, "Ctrl-C a trimis SIGINT grupului (wr %d, status %x)", (int)wr, stt);
    close(m);
    close(s);

    CHECK(openpty(&m, &s, NULL, NULL, &ws) == 0, "openpty pentru ysh");
    pid = fork();
    if(pid == 0){
        close(m);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if(s > 2){
            close(s);
        }
        char *av[] = {"ysh", NULL};
        execv("/bin/ysh", av);
        _exit(127);
    }
    close(s);
    char out[2048];
    msleep(300);
    write(m, "echo ptytest-$$\r", 16);
    read_until(m, out, sizeof(out), "ptytest-", 8000);
    CHECK(strstr(out, "ptytest-") != NULL, "ysh a raspuns pe pty: %.60s", out);
    msleep(200);
    write(m, "exit\r", 5);
    end = ms() + 8000;
    wr = 0;
    while(ms() < end && (wr = waitpid(pid, &stt, WNOHANG)) == 0){
        read_until(m, out, sizeof(out), NULL, 50);
    }
    CHECK(wr == pid && WIFEXITED(stt) && WEXITSTATUS(stt) == 0, "ysh a iesit cu 0 (wr %d, status %x)", (int)wr, stt);
    close(m);

    int ms_[10], ss_[10], opened = 0;
    errno = 0;
    for(int i = 0; i < 10; i++){
        if(openpty(&ms_[i], &ss_[i], NULL, NULL, NULL) < 0){
            break;
        }
        opened++;
    }
    CHECK(opened == 8 || opened == 7, "limita de pty-uri: %d deschise, errno %d", opened, errno);
    CHECK(errno == ENOSPC || errno == EMFILE, "dincolo de limita: errno %d", errno);
    for(int i = 0; i < opened; i++){
        close(ms_[i]);
        close(ss_[i]);
    }
    CHECK(openpty(&m, &s, NULL, NULL, NULL) == 0, "dupa inchidere se pot deschide din nou");
    close(m);
    close(s);
}

/* ================= B. ora ================= */

static void test_time(void){
    time_t before = time(NULL);
    struct timeval tv = {before + 3600, 0};
    CHECK(settimeofday(&tv, NULL) == 0, "settimeofday: errno %d", errno);
    time_t after = time(NULL);
    CHECK(after >= before + 3600 && after < before + 3700, "ora a sarit cu o ora (%ld -> %ld)", (long)before, (long)after);
    struct timespec ts = {before + 5, 0};
    CHECK(clock_settime(CLOCK_REALTIME, &ts) == 0, "clock_settime");
    CHECK(time(NULL) >= before + 5 && time(NULL) < before + 100, "clock_settime a mutat ceasul");
    struct timeval bad = {-1, 0};
    errno = 0;
    CHECK(settimeofday(&bad, NULL) < 0 && errno == EINVAL, "timp negativ -> EINVAL");
    tv.tv_sec = before;
    settimeofday(&tv, NULL);
}

/* ================= C. sesiunea GUI ================= */

static YwConn *ctl;
static int sw, sh;
static int cur_x, cur_y, btns;
static const YgTheme *th = &yg_theme_dark;
static int hold;

static void pump(int t){
    long long end = ms() + t;
    for(;;){
        long long left = end - ms();
        YwEvent ev;
        int r = yw_next_event(ctl, &ev, left > 0 ? (int)left : 0);
        if(r <= 0){
            return;
        }
    }
}

#define WAIT(cond, t) ({ \
    long long _e = ms() + (t); \
    int _ok = 0; \
    for(;;){ \
        if(cond){ _ok = 1; break; } \
        if(ms() > _e){ break; } \
        pump(20); \
    } \
    _ok; })

static void shot(const char *name){
    if(hold){
        pump(300);
        printf("appstest: shot %s\n", name);
        pump(3000);
    }
}

static void q(int kind, int a, int b, int32_t v[6]){
    memset(v, 0, 6 * sizeof(int32_t));
    yw_query(ctl, kind, a, b, v);
}

static int panel_q(int i){
    int32_t v[6];
    q(YW_Q_PANEL, 0, 0, v);
    return v[i];
}

static int wm_q(int i){
    int32_t v[6];
    q(YW_Q_WM, 0, 0, v);
    return v[i];
}

static uint32_t pixel(int x, int y){
    int32_t v[6];
    q(YW_Q_PIXEL, x, y, v);
    return (uint32_t)v[0];
}

static int theme_light(void){
    int32_t v[6];
    q(YW_Q_THEME, 0, 0, v);
    return v[0];
}

static void content(uint32_t id, int r[4]){
    int32_t v[6];
    q(YW_Q_WIN, (int)id, 0, v);
    for(int i = 0; i < 4; i++){
        r[i] = v[i];
    }
}

static int win_damage(uint32_t id){
    int32_t v[6];
    q(YW_Q_WIN, (int)id, 0, v);
    return v[4];
}

static int clampi(int v, int lo, int hi){
    return v < lo ? lo : v > hi ? hi : v;
}

static void inject_mouse(int dx, int dy){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_MOUSE;
    e.dx = (int16_t)dx;
    e.dy = (int16_t)dy;
    e.buttons = (uint8_t)btns;
    inputctl(INPUTCTL_INJECT, &e);
}

static void mouse_to(int x, int y){
    x = clampi(x, 0, sw - 1);
    y = clampi(y, 0, sh - 1);
    while(cur_x != x || cur_y != y){
        int dx = clampi(x - cur_x, -100, 100), dy = clampi(y - cur_y, -100, 100);
        inject_mouse(dx, dy);
        cur_x += dx;
        cur_y += dy;
    }
}

static int cursor_synced(void){
    int32_t v[6];
    q(YW_Q_STATS2, 0, 0, v);
    return v[0] == cur_x && v[1] == cur_y;
}

static void click(int x, int y){
    mouse_to(x, y);
    int ok = WAIT(cursor_synced(), 3000);
    CHECK(ok, "cursorul a ajuns la %d,%d", x, y);
    btns = YK_BTN_LEFT;
    inject_mouse(0, 0);
    btns = 0;
    inject_mouse(0, 0);
    pump(80);
}

static void key_event(int code, int ascii, int pressed, int mods){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_KEY;
    e.keycode = (uint16_t)code;
    e.ascii = (uint8_t)ascii;
    e.pressed = (uint8_t)pressed;
    e.modifiers = (uint8_t)mods;
    inputctl(INPUTCTL_INJECT, &e);
}

static void tap(int code, int ascii, int mods){
    key_event(code, ascii, 1, mods);
    key_event(code, ascii, 0, mods);
    pump(30);
}

static void type_str(const char *s){
    for(; *s; s++){
        tap(K_A, *s, 0);
    }
    pump(60);
}

static void ctrl_tap(int ascii){
    tap(K_A, ascii, YK_MOD_CTRL);
}

static void win_q_close(void){
    key_event(82, 0, 1, 0);
    key_event(16, 'q', 1, 0);
    key_event(16, 'q', 0, 0);
    key_event(82, 0, 0, 0);
}

static int file_has(const char *path, const char *text){
    char b[256];
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return 0;
    }
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if(n < 0){
        return 0;
    }
    b[n] = 0;
    return strstr(b, text) != NULL;
}

static int is_dir(const char *p){
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *p){
    struct stat st;
    return stat(p, &st) == 0;
}

static void write_file(const char *path, const char *text){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0){
        write(fd, text, strlen(text));
        close(fd);
    }
}

static int pin_x(int i){
    return PIN_X + i * (PIN_W + PIN_GAP) + PIN_W / 2;
}

static int bar_cy(void){
    return sh - PANEL_H / 2;
}

/* Linie orizontala de culoarea textului (bara video invers a yedit) in fereastra id? */
static int has_inverse_bar(uint32_t id){
    int r[4];
    content(id, r);
    uint32_t want = th->text & 0xFFFFFF;
    for(int y = r[1] + r[3] - 60; y < r[1] + r[3] - 2; y += 3){
        int hit = 0;
        for(int k = 1; k <= 8; k++){
            hit += (pixel(r[0] + r[2] * k / 9, y) & 0xFFFFFF) == want;
        }
        if(hit >= 7){
            return 1;
        }
    }
    return 0;
}

static int type_until(const char *line, const char *file, const char *needle, int tries){
    for(int i = 0; i < tries; i++){
        type_str(line);
        tap(K_ENTER, 0, 0);
        if(WAIT(file_has(file, needle), 1500)){
            return 1;
        }
    }
    return 0;
}

static void test_session(void){
    mkdir("/tmp/ytest", 0755);
    mkdir("/tmp/ytest/sub", 0755);
    write_file("/tmp/ytest/a.txt", "alfa\n");
    write_file("/tmp/ytest/b.txt", "beta\n");
    unlink("/tmp/t1");

    pid_t init = fork();
    if(init == 0){
        char *av[] = {"init", "-g", "-1", NULL};
        execv("/sbin/init", av);
        _exit(127);
    }
    for(int i = 0; i < 300 && !ctl; i++){
        ctl = yw_connect(NULL);
        if(!ctl){
            msleep(50);
        }
    }
    CHECK(ctl != NULL, "init -g a pornit compozitorul (errno %d)", errno);
    if(!ctl){
        kill(init, SIGKILL);
        waitpid(init, NULL, 0);
        return;
    }
    sw = yw_screen_width(ctl);
    sh = yw_screen_height(ctl);
    int ok = WAIT(panel_q(0) == PANEL_H, 10000);
    CHECK(ok, "sesiunea grafica a pornit si panoul (inaltime %d)", panel_q(0));
    {
        int32_t s[6];
        q(YW_Q_STATS2, 0, 0, s);
        cur_x = s[0];
        cur_y = s[1];
    }
    CHECK(!theme_light(), "tema implicita: Dark");

    /* --- yterm --- */
    click(pin_x(0), bar_cy());
    ok = WAIT(panel_q(5) == 1, 10000);
    CHECK(ok, "Terminalul pornit din bara (ferestre %d)", panel_q(5));
    uint32_t term = (uint32_t)wm_q(4);
    CHECK(term != 0, "Terminalul are focus");
    shot("terminal");
    ok = type_until("echo yterm-ok > /tmp/t1", "/tmp/t1", "yterm-ok", 8);
    CHECK(ok, "comanda tastata in yterm a rulat (ysh pe pty)");
    CHECK(WAIT(win_damage(term) >= 3, 3000), "yterm deseneaza iesirea (%d cadre)", win_damage(term));

    type_str("yedit /tmp/vt1");
    tap(K_ENTER, 0, 0);
    ok = WAIT(has_inverse_bar(term), 10000);
    CHECK(ok, "yedit deseneaza bara in video invers");
    shot("yedit");
    type_str("salut");
    tap(K_ENTER, 0, 0);
    type_str("lume");
    ctrl_tap('s');
    ok = WAIT(file_has("/tmp/vt1", "salut"), 5000) && file_has("/tmp/vt1", "lume");
    CHECK(ok, "yedit a salvat fisierul prin Ctrl-S");
    ctrl_tap('x');
    ok = WAIT(!has_inverse_bar(term), 5000);
    CHECK(ok, "dupa Ctrl-X yedit iese, ecranul revine la shell");
    unlink("/tmp/t2");
    ok = type_until("echo dupa-yedit > /tmp/t2", "/tmp/t2", "dupa-yedit", 5);
    CHECK(ok, "shell-ul raspunde dupa yedit");
    unlink("/tmp/t3");
    type_str("sleep 30; echo oops > /tmp/t3");
    tap(K_ENTER, 0, 0);
    msleep(500);
    ctrl_tap('c');
    msleep(500);
    ok = type_until("echo dupa-ctrlc > /tmp/t3", "/tmp/t3", "dupa-ctrlc", 6);
    CHECK(ok && !file_has("/tmp/t3", "oops"), "Ctrl-C opreste `sleep 30` din terminal");
    type_str("exit");
    tap(K_ENTER, 0, 0);
    ok = WAIT(panel_q(5) == 0, 10000);
    CHECK(ok, "`exit` inchide fereastra terminalului (ferestre %d)", panel_q(5));

    /* --- yfiles --- */
    click(pin_x(1), bar_cy());
    ok = WAIT(panel_q(5) == 1, 10000);
    CHECK(ok, "yfiles pornit din bara (ferestre %d)", panel_q(5));
    uint32_t files = (uint32_t)wm_q(4);
    pump(500);
    ctrl_tap('l');
    type_str("/tmp/ytest");
    tap(K_ENTER, 0, 0);
    pump(300);
    shot("yfiles");
    tap(K_DOWN, 0, 0);
    tap(K_DOWN, 0, 0);
    ctrl_tap('c');
    ctrl_tap('v');
    ok = WAIT(file_has("/tmp/ytest/a (copie).txt", "alfa"), 10000);
    CHECK(ok, "Ctrl+C / Ctrl+V a copiat a.txt in \"a (copie).txt\"");
    tap(K_F2, 0, 0);
    for(int i = 0; i < 20; i++){
        tap(K_BS, 0, 0);
    }
    type_str("z.txt");
    tap(K_ENTER, 0, 0);
    ok = WAIT(exists("/tmp/ytest/z.txt"), 5000) && !exists("/tmp/ytest/a (copie).txt");
    CHECK(ok, "F2: redenumit in z.txt");
    tap(K_DEL, 0, 0);
    pump(200);
    CHECK(exists("/tmp/ytest/z.txt"), "Delete cere confirmare");
    shot("yfiles-confirm");
    tap(K_ESC, 0, 0);
    pump(200);
    CHECK(exists("/tmp/ytest/z.txt"), "Esc renunta la stergere");
    tap(K_DEL, 0, 0);
    tap(K_ENTER, 0, 0);
    ok = WAIT(!exists("/tmp/ytest/z.txt"), 10000);
    CHECK(ok, "Delete + Enter a sters z.txt");
    CHECK(exists("/tmp/ytest/a.txt") && exists("/tmp/ytest/b.txt"), "celelalte fisiere au ramas");
    ctrl_tap('n');
    for(int i = 0; i < 20; i++){
        tap(K_BS, 0, 0);
    }
    type_str("dnou");
    tap(K_ENTER, 0, 0);
    ok = WAIT(is_dir("/tmp/ytest/dnou"), 5000);
    CHECK(ok, "Ctrl+N: dosar nou");
    type_str("b");
    ctrl_tap('x');
    tap(K_HOME, 0, 0);
    tap(K_ENTER, 0, 0);
    pump(300);
    ctrl_tap('v');
    ok = WAIT(file_has("/tmp/ytest/dnou/b.txt", "beta"), 10000) && !exists("/tmp/ytest/b.txt");
    CHECK(ok, "Ctrl+X / Ctrl+V a mutat b.txt in dnou");
    tap(K_BS, 0, 0);
    pump(200);
    pump(1200);                              /* expira prefixul tastat ("b") */
    type_str("a");
    tap(K_F4, 0, 0);
    ok = WAIT(panel_q(5) == 2, 10000);
    CHECK(ok, "F4 deschide yedit intr-un yterm (ferestre %d)", panel_q(5));
    uint32_t ed = (uint32_t)wm_q(4);
    CHECK(ed != files && ed != 0, "fereastra yedit are focus");
    ok = WAIT(has_inverse_bar(ed), 10000);
    CHECK(ok, "yedit ruleaza in fereastra deschisa de yfiles");
    ctrl_tap('x');
    ok = WAIT(panel_q(5) == 1, 10000);
    CHECK(ok, "Ctrl-X in yedit inchide si fereastra yterm (ferestre %d)", panel_q(5));
    win_q_close();
    ok = WAIT(panel_q(5) == 0, 8000);
    CHECK(ok, "Win+Q inchide yfiles (ferestre %d)", panel_q(5));

    /* --- ysettings --- */
    click(pin_x(2), bar_cy());
    ok = WAIT(panel_q(5) == 1, 10000);
    CHECK(ok, "ysettings pornit din bara (ferestre %d)", panel_q(5));
    pump(500);
    shot("settings-dark");
    uint32_t bar_dark = yg_theme_dark.surface_alt & 0xFFFFFF, bar_light = yg_theme_light.surface_alt & 0xFFFFFF;
    ok = WAIT((pixel(5, sh - 2) & 0xFFFFFF) == bar_dark, 3000);
    CHECK(ok, "bara in tema Dark: %06x", (unsigned)pixel(5, sh - 2));
    tap(K_RIGHT, 0, 0);
    ok = WAIT(theme_light() == 1, 5000);
    CHECK(ok, "ysettings: Dreapta = tema Light (compozitorul)");
    ok = WAIT((pixel(5, sh - 2) & 0xFFFFFF) == bar_light, 5000);
    CHECK(ok, "panoul a primit YW_EV_THEME: bara %06x", (unsigned)pixel(5, sh - 2));
    th = &yg_theme_light;
    shot("settings-light");
    tap(K_LEFT, 0, 0);
    ok = WAIT(theme_light() == 0 && (pixel(5, sh - 2) & 0xFFFFFF) == bar_dark, 5000);
    CHECK(ok, "ysettings: Stanga = tema Dark");
    th = &yg_theme_dark;
    tap(K_F2, 0, 0);
    pump(300);
    shot("settings-datetime");
    time_t t0 = time(NULL);
    tap(K_UP, 0, 0);
    tap(K_ENTER, 0, 0);
    ok = WAIT(time(NULL) > t0 + 300LL * 86400, 5000);
    CHECK(ok, "Data si ora: anul +1 aplicat prin settimeofday (%ld -> %ld)", (long)t0, (long)time(NULL));
    {
        struct timeval tv = {t0 + 10, 0};
        settimeofday(&tv, NULL);
    }
    tap(K_F3, 0, 0);
    pump(400);
    shot("settings-network");
    tap(K_F4, 0, 0);
    pump(400);
    shot("settings-users");
    tap(K_F5, 0, 0);
    pump(300);
    shot("settings-telemetry");
    CHECK(panel_q(5) == 1, "ysettings ruleaza dupa toate paginile (ferestre %d)", panel_q(5));
    win_q_close();
    ok = WAIT(panel_q(5) == 0, 8000);
    CHECK(ok, "Win+Q inchide ysettings (ferestre %d)", panel_q(5));

    /* --- D. Ctrl+Alt+Backspace --- */
    yw_disconnect(ctl);
    ctl = NULL;
    msleep(200);
    key_event(K_BS, 0, 1, YK_MOD_CTRL | YK_MOD_ALT);
    key_event(K_BS, 0, 0, YK_MOD_CTRL | YK_MOD_ALT);
    int st = -1;
    long long end = ms() + 15000;
    pid_t w = 0;
    while(ms() < end && (w = waitpid(init, &st, WNOHANG)) == 0){
        msleep(50);
    }
    CHECK(w == init && WIFEXITED(st) && WEXITSTATUS(st) == 0, "init -g -1 iese cu 0 dupa Ctrl+Alt+Backspace (w %d, status %x)", (int)w, st);
    if(w != init){
        kill(init, SIGKILL);
        waitpid(init, NULL, 0);
    }
    msleep(500);
    while(waitpid(-1, NULL, WNOHANG) > 0){
    }
    /* fisierele din tmpfs tin pagini: curata ca testul de scurgeri din kernel sa nu le numere */
    const char *junk[] = {"/tmp/t1", "/tmp/t2", "/tmp/t3", "/tmp/vt1", "/tmp/ytest/a.txt", "/tmp/ytest/b.txt", "/tmp/ytest/dnou/b.txt",
                          "/tmp/ytest/z.txt", "/tmp/ytest/a (copie).txt", "/tmp/.yedit.swp", NULL};
    for(int i = 0; junk[i]; i++){
        unlink(junk[i]);
    }
    rmdir("/tmp/ytest/dnou");
    rmdir("/tmp/ytest/sub");
    rmdir("/tmp/ytest");
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    hold = access("/etc/appshold", F_OK) == 0;
    test_pty();
    test_time();
    test_session();
    printf("appstest: %d verificari, %d esecuri\n", checks, fails);
    if(fails){
        return 1;
    }
    printf("[OK] appstest\n");
    return 0;
}
