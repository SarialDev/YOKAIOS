/* comptest (T10.2): verificari pentru compozitor si pentru ce ii trebuie din kernel. Rulat de test=comp, iese cu 0
 * daca totul trece.
 *   1. kernel: shm_open/ftruncate/mmap intre procese neinrudite prin nume, AF_UNIX (socketpair, bind/listen/connect/
 *      accept, nume ocupat, refuz, neblocant, EOF, EPIPE, transfer mare), poll(-1) care doarme si se trezeste,
 *      inputctl (captura exclusiva, injectare), rt_stats
 *   2. ycomp cu doi clienti (procese separate, comandati prin pipe-uri): se actualizeaza independent si impreuna
 *      (pixelii din back buffer, YW_DAMAGE / YW_FRAME numarate), focus + rutarea tastelor si clicurilor (mouse si
 *      taste injectate), zero cadre si cel mult doua treziri intr-o secunda fara schimbari, 30 FPS fara animatii sub
 *      un task SCHED_FIFO care ia 90% dintr-un CPU si revenirea la 60 FPS dupa, butonul de inchidere, iesirea curata. */
#include "ywin.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtstats.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/yinput.h>

static int checks, fails;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)){ \
        fails++; \
        printf("comptest: FAIL %s:%d: ", __FILE__, __LINE__); \
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

/* ================= 1. kernel ================= */

static void test_shm(void){
    const char *name = "/comptest-shm";
    shm_unlink(name);
    errno = 0;
    CHECK(shm_open(name, O_RDWR, 0) < 0 && errno == ENOENT, "shm_open fara O_CREAT: errno %d", errno);
    errno = 0;
    CHECK(shm_open("fara-slash", O_RDWR | O_CREAT, 0600) < 0 && errno == EINVAL, "nume invalid: errno %d", errno);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0, "shm_open O_CREAT: errno %d", errno);
    errno = 0;
    CHECK(shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600) < 0 && errno == EEXIST, "O_EXCL: errno %d", errno);
    CHECK(ftruncate(fd, 3 * 4096 + 100) == 0, "ftruncate: errno %d", errno);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 3 * 4096 + 100 && S_ISREG(st.st_mode), "fstat: %ld", (long)st.st_size);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EINVAL, "MAP_PRIVATE: errno %d", errno);
    errno = 0;
    CHECK(mmap(NULL, 5 * 4096, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENXIO, "peste capat: errno %d", errno);
    uint32_t *p = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "mmap: errno %d", errno);
    if(p == MAP_FAILED){
        return;
    }
    CHECK(p[0] == 0 && p[4095] == 0, "paginile noi sunt zero");
    /* Un copil deschide obiectul DUPA NUME (nu mosteneste maparea: exec-ul ar sterge-o, aici o face munmap) */
    int sync[2];
    pipe(sync);
    pid_t pid = fork();
    if(pid == 0){
        char go;
        read(sync[0], &go, 1);
        int f2 = shm_open(name, O_RDWR, 0);
        uint32_t *q = f2 >= 0 ? mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f2, 0) : MAP_FAILED;
        if(q == MAP_FAILED){
            _exit(1);
        }
        int ok = q[10] == 0x11223344u;
        q[2000] = 0xCAFEBABEu;
        q[3 * 1024 + 5] = 0x55u;
        _exit(ok ? 0 : 2);
    }
    p[10] = 0x11223344u;
    write(sync[1], "g", 1);
    int stc = -1;
    waitpid(pid, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "copilul a vazut scrierea parintelui: %d", stc);
    CHECK(p[2000] == 0xCAFEBABEu && p[3 * 1024 + 5] == 0x55u, "parintele vede scrierea copilului: %x", p[2000]);
    char buf[4];
    CHECK(lseek(fd, 2000 * 4, SEEK_SET) == 8000 && read(fd, buf, 4) == 4 && !memcmp(buf, &p[2000], 4), "read pe fd shm");
    close(sync[0]);
    close(sync[1]);
    CHECK(shm_unlink(name) == 0, "shm_unlink");
    errno = 0;
    CHECK(shm_open(name, O_RDWR, 0) < 0 && errno == ENOENT, "dupa unlink: errno %d", errno);
    p[1] = 7;                                       /* maparea ramane valida dupa unlink si close */
    close(fd);
    CHECK(p[1] == 7, "mapare dupa close");
    munmap(p, 4 * 4096);
}

static void test_unix(void){
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair: errno %d", errno);
    char b[64];
    CHECK(write(sv[0], "salut", 5) == 5 && read(sv[1], b, sizeof(b)) == 5 && !memcmp(b, "salut", 5), "socketpair echo");
    struct stat st;
    CHECK(fstat(sv[0], &st) == 0 && S_ISSOCK(st.st_mode), "fstat socket");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    errno = 0;
    CHECK(read(sv[1], b, 1) < 0 && errno == EAGAIN, "neblocant gol: errno %d", errno);
    /* poll(-1) doarme pana scrie copilul (fara sondare) */
    pid_t pid = fork();
    if(pid == 0){
        msleep(60);
        write(sv[0], "x", 1);
        _exit(0);
    }
    long long t0 = ms();
    struct pollfd pf = {sv[1], POLLIN, 0};
    int r = poll(&pf, 1, -1);
    long long dt = ms() - t0;
    CHECK(r == 1 && (pf.revents & POLLIN) && dt >= 40, "poll(-1) pe AF_UNIX: r=%d dt=%lld", r, dt);
    waitpid(pid, NULL, 0);
    read(sv[1], b, 1);
    close(sv[0]);
    CHECK(read(sv[1], b, 1) == 0, "EOF dupa close");
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP), "POLLHUP: %x", pf.revents);
    close(sv[1]);

    /* poll cu timeout pe un pipe gol: expira, fara sa se trezeasca inainte */
    int pp[2];
    pipe(pp);
    pf.fd = pp[0];
    t0 = ms();
    r = poll(&pf, 1, 120);
    dt = ms() - t0;
    CHECK(r == 0 && dt >= 110, "poll(120 ms) pe pipe: r=%d dt=%lld", r, dt);
    close(pp[0]);
    close(pp[1]);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/run/comptest.sock");
    int c0 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    CHECK(connect(c0, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno == ECONNREFUSED, "fara server: errno %d", errno);
    close(c0);
    errno = 0;
    CHECK(socket(AF_UNIX, SOCK_DGRAM, 0) < 0 && errno == EPROTONOSUPPORT, "SOCK_DGRAM: errno %d", errno);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(l >= 0 && bind(l, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(l, 4) == 0, "bind/listen: errno %d", errno);
    int l2 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    CHECK(bind(l2, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno == EADDRINUSE, "nume ocupat: errno %d", errno);
    close(l2);
    struct sockaddr_un got;
    socklen_t gl = sizeof(got);
    CHECK(getsockname(l, (struct sockaddr *)&got, &gl) == 0 && !strcmp(got.sun_path, "/run/comptest.sock"),
          "getsockname");
    /* Copilul trimite 200000 de octeti (bufferul are 64 KiB: scriitorul asteapta cititorul) si citeste ecoul sumei */
    pid = fork();
    if(pid == 0){
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if(connect(c, (struct sockaddr *)&sa, sizeof(sa)) < 0){
            _exit(1);
        }
        static unsigned char big[200000];
        for(int i = 0; i < (int)sizeof(big); i++){
            big[i] = (unsigned char)(i * 31 + 7);
        }
        if(write(c, big, sizeof(big)) != (ssize_t)sizeof(big)){
            _exit(2);
        }
        shutdown(c, SHUT_WR);
        unsigned sum = 0;
        if(read(c, &sum, sizeof(sum)) != sizeof(sum)){
            _exit(3);
        }
        unsigned want = 0;
        for(int i = 0; i < (int)sizeof(big); i++){
            want += big[i];
        }
        _exit(sum == want ? 0 : 4);
    }
    int a = accept(l, NULL, NULL);
    CHECK(a >= 0, "accept: errno %d", errno);
    unsigned sum = 0;
    long total = 0;
    for(;;){
        unsigned char chunk[3000];
        ssize_t n = read(a, chunk, sizeof(chunk));
        if(n <= 0){
            break;
        }
        for(ssize_t i = 0; i < n; i++){
            sum += chunk[i];
        }
        total += n;
    }
    CHECK(total == 200000, "transfer: %ld octeti", total);
    write(a, &sum, sizeof(sum));
    int stc = -1;
    waitpid(pid, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "clientul AF_UNIX: %d", stc);
    errno = 0;
    CHECK(send(a, "z", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "EPIPE: errno %d", errno);
    close(a);
    close(l);
    /* Dupa close numele e liber */
    l = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(l, (struct sockaddr *)&sa, sizeof(sa)) == 0, "nume eliberat la close: errno %d", errno);
    close(l);
}

static void test_input(void){
    int fd = inputctl(INPUTCTL_GRAB, NULL);
    CHECK(fd >= 0, "INPUTCTL_GRAB: errno %d", errno);
    errno = 0;
    CHECK(inputctl(INPUTCTL_GRAB, NULL) < 0 && errno == EBUSY, "a doua captura: errno %d", errno);
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_KEY;
    e.keycode = 30;
    e.ascii = 'a';
    e.pressed = 1;
    CHECK(inputctl(INPUTCTL_INJECT, &e) == 0, "INJECT tasta");
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_MOUSE;
    e.dx = 5;
    e.dy = -3;
    e.buttons = YK_BTN_LEFT;
    CHECK(inputctl(INPUTCTL_INJECT, &e) == 0, "INJECT mouse");
    struct pollfd pf = {fd, POLLIN, 0};
    CHECK(poll(&pf, 1, 1000) == 1, "POLLIN pe fd-ul de input");
    struct yk_input_event got[4];
    ssize_t n = read(fd, got, sizeof(got));
    CHECK(n == 2 * (ssize_t)sizeof(got[0]), "doua evenimente: %ld", (long)n);
    if(n == 2 * (ssize_t)sizeof(got[0])){
        CHECK(got[0].type == YK_EV_KEY && got[0].ascii == 'a' && got[0].pressed && got[0].keycode == 30, "tasta");
        CHECK(got[1].type == YK_EV_MOUSE && got[1].dx == 5 && got[1].dy == -3 && got[1].buttons == YK_BTN_LEFT, "mouse");
    }
    close(fd);
    fd = inputctl(INPUTCTL_GRAB, NULL);
    CHECK(fd >= 0, "captura din nou dupa close");
    close(fd);
}

static void test_rtstats(void){
    struct yk_rt_stats s;
    CHECK(rt_stats(&s) == 0 && s.ncpu >= 1, "rt_stats: errno %d", errno);
    errno = 0;
    CHECK(rt_stats((struct yk_rt_stats *)8) < 0 && errno == EFAULT, "rt_stats EFAULT");
}

/* ================= 2. compozitorul ================= */

#define SOCK "/run/comptest-ycomp.sock"
#define WW 300
#define WH 200

static YgColor base_color(int who){
    return who == 0 ? YG_RGB(0x20, 0x60, 0xC0) : YG_RGB(0xC0, 0x80, 0x20);
}

static YgColor sq_color(int who, int i){
    return YG_RGB((i * 7) & 0xFF, who ? 0x30 : 0xD0, (50 + i) & 0xFF);
}

static void report(int fd, const char *fmt, int a, int b){
    char line[64];
    int n = snprintf(line, sizeof(line), fmt, a, b);
    write(fd, line, (size_t)n);
}

/* Linie de comanda de la parinte (neblocant: 1 = linie, 0 = nimic, -1 = EOF) */
static int read_cmd(int fd, char *buf, int *len, char *out){
    for(;;){
        char c;
        ssize_t n = read(fd, &c, 1);
        if(n == 0){
            return -1;
        }
        if(n < 0){
            return 0;
        }
        if(c == '\n'){
            buf[*len] = 0;
            strcpy(out, buf);
            *len = 0;
            return 1;
        }
        if(*len < 60){
            buf[(*len)++] = c;
        }
    }
}

static int step(YwWindow *w, int who, int i){
    YgSurface *s = yw_window_surface(w);
    yg_fill_rect(s, 10, 10, 50, 50, sq_color(who, i));
    yw_window_damage(w, 10, 10, 50, 50);
    return yw_window_wait_frame(w, 5000);
}

static int handle_event(YwConn *c, int rep, YwEvent *ev){
    (void)c;
    if(ev->type == YW_EV_KEY && ev->pressed && ev->ascii){
        report(rep, "K %c\n", ev->ascii, 0);
    } else if(ev->type == YW_EV_MOUSE && ev->kind == YW_MOUSE_PRESS){
        report(rep, "M %d %d\n", ev->x, ev->y);
    } else if(ev->type == YW_EV_FOCUS){
        report(rep, "F %d\n", ev->focus, 0);
    } else if(ev->type == YW_EV_CLOSE){
        report(rep, "X\n", 0, 0);
        return 1;
    }
    return 0;
}

/* Clientul who (0 = A, 1 = B): fereastra la (px, py), comenzi pe cmd, rapoarte pe rep */
static int client_main(int who, int px, int py, int cmd, int rep){
    YwConn *c = NULL;
    for(int i = 0; i < 100 && !c; i++){
        c = yw_connect(SOCK);
        if(!c){
            msleep(50);
        }
    }
    if(!c){
        return 10;
    }
    YwWindow *w = yw_window_new(c, WW, WH, who ? "Fereastra B" : "Fereastra A");
    if(!w){
        return 11;
    }
    yw_window_move(w, px, py);
    yg_clear(yw_window_surface(w), base_color(who));
    yw_window_damage_all(w);
    if(yw_window_wait_frame(w, 5000) < 0){
        return 12;
    }
    report(rep, "R %d\n", (int)yw_window_id(w), 0);
    fcntl(cmd, F_SETFL, O_NONBLOCK);
    char lb[64], line[64];
    int ll = 0;
    int continuous = 0, cont_n = 0, frame_i = 1000;
    for(;;){
        YwEvent ev;
        while(yw_pending(c)){
            if(yw_next_event(c, &ev, 0) == 1 && handle_event(c, rep, &ev)){
                yw_window_destroy(w);
                yw_disconnect(c);
                return 0;
            }
        }
        struct pollfd pf[2] = {{yw_fd(c), POLLIN, 0}, {cmd, POLLIN, 0}};
        int r = poll(pf, 2, continuous ? 0 : -1);
        if(r < 0 && errno != EINTR){
            return 13;
        }
        if(pf[0].revents){
            int k = yw_next_event(c, &ev, 0);
            if(k < 0){
                return 14;
            }
            if(k == 1 && handle_event(c, rep, &ev)){
                yw_window_destroy(w);
                yw_disconnect(c);
                return 0;
            }
        }
        if(pf[1].revents){
            int k;
            while((k = read_cmd(cmd, lb, &ll, line)) == 1){
                if(line[0] == 'A'){
                    int n = atoi(line + 2);
                    int ok = 1;
                    for(int i = 0; i < n; i++){
                        ok &= step(w, who, i) == 0;
                    }
                    report(rep, ok ? "D %d\n" : "E %d\n", n, 0);
                } else if(line[0] == 'C'){
                    continuous = 1;
                    cont_n = 0;
                } else if(line[0] == 'S'){
                    continuous = 0;
                    report(rep, "S %d\n", cont_n, 0);
                } else if(line[0] == 'Q'){
                    yw_window_destroy(w);
                    yw_disconnect(c);
                    return 0;
                }
            }
            if(k < 0){
                return 15;
            }
        }
        if(continuous){
            if(step(w, who, frame_i++ & 0xFF) == 0){
                cont_n++;
            }
        }
    }
}

typedef struct {
    pid_t pid;
    int cmd, rep;
    char buf[256];
    int len;
    uint32_t id;
} Peer;

static pid_t start_client(Peer *p, int who, int px, int py){
    int a[2], b[2];
    pipe(a);
    pipe(b);
    pid_t pid = fork();
    if(pid == 0){
        close(a[1]);
        close(b[0]);
        _exit(client_main(who, px, py, a[0], b[1]));
    }
    close(a[0]);
    close(b[1]);
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->cmd = a[1];
    p->rep = b[0];
    return pid;
}

/* Urmatoarea linie de raport care incepe cu prefix (celelalte se arunca); 1, 0 la expirare */
static int expect(Peer *p, char prefix, char *out, int timeout_ms){
    long long end = ms() + timeout_ms;
    for(;;){
        char *nl = memchr(p->buf, '\n', (size_t)p->len);
        while(nl){
            *nl = 0;
            int match = p->buf[0] == prefix;
            if(match && out){
                strcpy(out, p->buf);
            }
            int used = (int)(nl - p->buf) + 1;
            memmove(p->buf, p->buf + used, (size_t)(p->len - used));
            p->len -= used;
            if(match){
                return 1;
            }
            nl = memchr(p->buf, '\n', (size_t)p->len);
        }
        long long left = end - ms();
        if(left <= 0){
            return 0;
        }
        struct pollfd pf = {p->rep, POLLIN, 0};
        if(poll(&pf, 1, (int)left) <= 0){
            continue;
        }
        ssize_t n = read(p->rep, p->buf + p->len, sizeof(p->buf) - 1 - (size_t)p->len);
        if(n <= 0){
            return 0;
        }
        p->len += (int)n;
    }
}

static void cmd(Peer *p, const char *s){
    write(p->cmd, s, strlen(s));
}

static YwConn *ctl;

static int32_t q1(int kind, int a, int b, int idx){
    int32_t v[6] = {0};
    if(yw_query(ctl, kind, a, b, v) < 0){
        return -999;
    }
    return v[idx];
}

static uint32_t pixel(int x, int y){
    return (uint32_t)q1(YW_Q_PIXEL, x, y, 0);
}

static void win_info(uint32_t id, int32_t v[6]){
    memset(v, 0, 6 * sizeof(int32_t));
    yw_query(ctl, YW_Q_WIN, (int)id, 0, v);
}

static void inject_mouse(int dx, int dy, int buttons){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_MOUSE;
    e.dx = (int16_t)dx;
    e.dy = (int16_t)dy;
    e.buttons = (uint8_t)buttons;
    inputctl(INPUTCTL_INJECT, &e);
}

static void inject_key(int keycode, int ascii){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_KEY;
    e.keycode = (uint16_t)keycode;
    e.ascii = (uint8_t)ascii;
    e.pressed = 1;
    inputctl(INPUTCTL_INJECT, &e);
    e.pressed = 0;
    e.ascii = 0;
    inputctl(INPUTCTL_INJECT, &e);
}

/* Muta cursorul la (x, y) si face clic stanga */
static void click_at(int x, int y){
    int cx = q1(YW_Q_STATS2, 0, 0, 0), cy = q1(YW_Q_STATS2, 0, 0, 1);
    inject_mouse(x - cx, y - cy, 0);
    inject_mouse(0, 0, YK_BTN_LEFT);
    inject_mouse(0, 0, 0);
}

static int wait_stats(int idx, int want, int timeout_ms){
    long long end = ms() + timeout_ms;
    while(ms() < end){
        if(q1(YW_Q_STATS, 0, 0, idx) == want){
            return 1;
        }
        msleep(50);
    }
    return q1(YW_Q_STATS, 0, 0, idx) == want;
}

static void rt_spinner(int run_ms){
    struct sched_param sp = {10};
    if(sched_setscheduler(0, SCHED_FIFO, &sp) < 0){
        _exit(1);
    }
    long long end = ms() + run_ms;
    while(ms() < end){
        long long t = ms() + 9;
        while(ms() < t){
        }
        msleep(1);
    }
    _exit(0);
}

static void test_compositor(void){
    pid_t comp = fork();
    if(comp == 0){
        char *const av[] = {"ycomp", "-s", SOCK, "-q", NULL};
        execv("/bin/ycomp", av);
        _exit(127);
    }
    for(int i = 0; i < 200 && !ctl; i++){
        ctl = yw_connect(SOCK);
        if(!ctl){
            msleep(50);
        }
    }
    CHECK(ctl != NULL, "conectare la ycomp: errno %d", errno);
    if(!ctl){
        kill(comp, SIGKILL);
        waitpid(comp, NULL, 0);
        return;
    }
    int sw = yw_screen_width(ctl), sh = yw_screen_height(ctl);
    CHECK(sw >= 640 && sh >= 480, "ecran %dx%d", sw, sh);
    CHECK(q1(YW_Q_STATS2, 0, 0, 5) == SCHED_OTHER, "ycomp ruleaza cu prioritate NORMAL");
    CHECK(q1(YW_Q_STATS, 0, 0, 2) == 60 && q1(YW_Q_STATS, 0, 0, 3) == 1, "60 FPS + animatii la pornire");

    Peer A, B;
    start_client(&A, 0, 60, 80);
    start_client(&B, 1, 560, 80);
    char line[256];
    CHECK(expect(&A, 'R', line, 15000), "A gata");
    A.id = (uint32_t)atoi(line + 2);
    CHECK(expect(&B, 'R', line, 15000), "B gata");
    B.id = (uint32_t)atoi(line + 2);
    msleep(400);                                    /* animatiile de deschidere se termina */
    CHECK(q1(YW_Q_STATS, 0, 0, 4) == 2, "doua ferestre: %d", q1(YW_Q_STATS, 0, 0, 4));

    int32_t wa[6], wb[6];
    win_info(A.id, wa);
    win_info(B.id, wb);
    CHECK(wa[0] == 60 + YW_BORDER && wa[1] == 80 + YW_TITLE_H && wa[2] == WW && wa[3] == WH, "A la %d,%d %dx%d",
          wa[0], wa[1], wa[2], wa[3]);
    CHECK(wb[0] == 560 + YW_BORDER && wb[2] == WW, "B la %d,%d", wb[0], wb[1]);
    int ax = wa[0], ay = wa[1], bx = wb[0], by = wb[1];
    CHECK(pixel(ax + 150, ay + 150) == (base_color(0) & 0xFFFFFF), "continutul lui A: %06x", pixel(ax + 150, ay + 150));
    CHECK(pixel(bx + 150, by + 150) == (base_color(1) & 0xFFFFFF), "continutul lui B: %06x", pixel(bx + 150, by + 150));
    CHECK(pixel(ax + 30, ay - YW_TITLE_H / 2) != (base_color(0) & 0xFFFFFF), "bara de titlu desenata");

    /* --- A se actualizeaza singur: B nu se schimba --- */
    int32_t b0[6];
    win_info(B.id, b0);
    cmd(&A, "A 30\n");
    int samples = 0, b_same = 1;
    long long end = ms() + 20000;
    int a_done = 0;
    while(!a_done && ms() < end){
        if(pixel(bx + 30, by + 30) != (base_color(1) & 0xFFFFFF)){
            b_same = 0;
        }
        samples++;
        a_done = expect(&A, 'D', line, 20);
    }
    CHECK(a_done && !strcmp(line, "D 30"), "A: 30 de cadre (%s)", a_done ? line : "timeout");
    CHECK(b_same && samples > 0, "B neschimbat cat timp A se anima (%d probe)", samples);
    CHECK(pixel(ax + 30, ay + 30) == (sq_color(0, 29) & 0xFFFFFF), "A: ultimul cadru pe ecran %06x",
          pixel(ax + 30, ay + 30));
    int32_t b1[6], a1[6];
    win_info(B.id, b1);
    win_info(A.id, a1);
    CHECK(b1[4] == b0[4] && b1[5] == b0[5], "B: niciun damage/cadru nou (%d/%d -> %d/%d)", b0[4], b0[5], b1[4], b1[5]);

    /* --- B se actualizeaza singur --- */
    cmd(&B, "A 10\n");
    CHECK(expect(&B, 'D', line, 20000) && !strcmp(line, "D 10"), "B: 10 cadre");
    CHECK(pixel(bx + 30, by + 30) == (sq_color(1, 9) & 0xFFFFFF), "B: ultimul cadru %06x", pixel(bx + 30, by + 30));
    CHECK(pixel(ax + 30, ay + 30) == (sq_color(0, 29) & 0xFFFFFF), "A ramane la cadrul lui");
    int32_t a2[6];
    win_info(A.id, a2);
    CHECK(a2[4] == a1[4] && a2[5] == a1[5], "A: niciun damage nou cat B se anima");

    /* --- amandoua deodata, fiecare in ritmul lui --- */
    int32_t b2[6];
    win_info(B.id, b2);
    cmd(&A, "A 20\n");
    cmd(&B, "A 25\n");
    CHECK(expect(&A, 'D', line, 30000) && !strcmp(line, "D 20"), "A: 20 cadre in paralel");
    CHECK(expect(&B, 'D', line, 30000) && !strcmp(line, "D 25"), "B: 25 cadre in paralel");
    int32_t a3[6], b3[6];
    win_info(A.id, a3);
    win_info(B.id, b3);
    CHECK(a3[4] - a2[4] == 20 && a3[5] - a2[5] == 20, "A: 20 damage / 20 YW_FRAME (%d/%d)", a3[4] - a2[4], a3[5] - a2[5]);
    CHECK(b3[4] - b2[4] == 25 && b3[5] - b2[5] == 25, "B: 25 damage / 25 YW_FRAME (%d/%d)", b3[4] - b2[4], b3[5] - b2[5]);
    CHECK(pixel(ax + 30, ay + 30) == (sq_color(0, 19) & 0xFFFFFF) && pixel(bx + 30, by + 30) == (sq_color(1, 24) & 0xFFFFFF),
          "ultimele cadre ale amandurora pe ecran");

    /* --- focus si input: clic pe A, tasta 'y' doar la A; clic pe B, 'x' doar la B --- */
    click_at(ax + WW / 2, ay + WH / 2);
    CHECK(expect(&A, 'M', line, 5000), "A: clic primit");
    CHECK(!strcmp(line, "M 150 100"), "A: clic la coordonate locale (%s)", line);
    CHECK(q1(YW_Q_STATS, 0, 0, 5) == (int)A.id, "focus pe A");
    inject_key(21, 'y');
    CHECK(expect(&A, 'K', line, 5000) && line[2] == 'y', "A: tasta y");
    CHECK(!expect(&B, 'K', line, 300), "B nu primeste tasta lui A");
    click_at(bx + 20, by + 40);
    CHECK(expect(&B, 'M', line, 5000) && !strcmp(line, "M 20 40"), "B: clic (%s)", line);
    CHECK(q1(YW_Q_STATS, 0, 0, 5) == (int)B.id, "focus pe B");
    inject_key(45, 'x');
    CHECK(expect(&B, 'K', line, 5000) && line[2] == 'x', "B: tasta x");
    CHECK(!expect(&A, 'K', line, 300), "A nu primeste tasta lui B");
    CHECK(q1(YW_Q_STATS2, 0, 0, 0) == bx + 20 && q1(YW_Q_STATS2, 0, 0, 1) == by + 40, "cursorul software urmeaza mouse-ul");
    CHECK(pixel(bx + 21, by + 41) == 0xFFFFFF || pixel(bx + 21, by + 42) == 0xFFFFFF, "sageata desenata peste B");

    /* --- nimic nu se schimba: zero cadre, CPU liber (bucla nu se trezeste) --- */
    msleep(500);
    int32_t s0[6], s1[6];
    yw_query(ctl, YW_Q_STATS, 0, 0, s0);
    msleep(1000);
    yw_query(ctl, YW_Q_STATS, 0, 0, s1);
    printf("comptest: inactiv 1 s: %d cadre, %d treziri\n", s1[0] - s0[0], s1[1] - s0[1]);
    CHECK(s1[0] == s0[0], "fara cadre cand nimic nu se schimba (%d)", s1[0] - s0[0]);
    CHECK(s1[1] - s0[1] <= 2, "treziri intr-o secunda inactiva: %d", s1[1] - s0[1]);

    /* --- incarcare RT: 30 FPS fara animatii, apoi inapoi la 60 --- */
    cmd(&A, "C\n");
    msleep(600);
    CHECK(q1(YW_Q_STATS, 0, 0, 2) == 60, "60 FPS inainte de RT");
    pid_t spin = fork();
    if(spin == 0){
        rt_spinner(3000);
    }
    int degraded = wait_stats(2, 30, 2500) && wait_stats(3, 0, 500);
    CHECK(degraded, "sub RT: %d FPS, animatii %d, incarcare %d ppm", q1(YW_Q_STATS, 0, 0, 2), q1(YW_Q_STATS, 0, 0, 3),
          q1(YW_Q_STATS2, 0, 0, 2));
    if(degraded){
        int32_t f0 = q1(YW_Q_STATS, 0, 0, 0);
        long long t0 = ms();
        msleep(1000);
        int32_t f1 = q1(YW_Q_STATS, 0, 0, 0);
        long long dt = ms() - t0;
        int rate = (int)((f1 - f0) * 1000LL / (dt ? dt : 1));
        printf("comptest: sub RT %d cadre/s (tinta 30)\n", rate);
        CHECK(rate <= 33 && f1 > f0, "cadre/s sub RT: %d", rate);
    }
    int stc = -1;
    waitpid(spin, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "task-ul RT: %d", stc);
    CHECK(wait_stats(2, 60, 5000) && wait_stats(3, 1, 500), "dupa RT: %d FPS, animatii %d", q1(YW_Q_STATS, 0, 0, 2),
          q1(YW_Q_STATS, 0, 0, 3));
    CHECK(q1(YW_Q_STATS2, 0, 0, 3) >= 1, "reducerea FPS numarata");
    cmd(&A, "S\n");
    CHECK(expect(&A, 'S', line, 10000), "A: animatia continua oprita (%s)", line);

    /* --- butonul de inchidere al lui A --- */
    win_info(A.id, wa);
    int cbx = wa[0] - YW_BORDER + WW + 2 * YW_BORDER - 6 - YW_CLOSE_SIZE / 2;
    int cby = wa[1] - YW_TITLE_H + YW_TITLE_H / 2;
    click_at(cbx, cby);
    CHECK(expect(&A, 'X', line, 5000), "A: YW_EV_CLOSE la butonul de inchidere");
    waitpid(A.pid, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "clientul A: %d", stc);
    CHECK(wait_stats(4, 1, 3000), "o fereastra dupa inchiderea lui A");
    CHECK(q1(YW_Q_STATS, 0, 0, 5) == (int)B.id, "focusul trece la B");
    cmd(&B, "Q\n");
    waitpid(B.pid, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "clientul B: %d", stc);
    CHECK(wait_stats(4, 0, 3000), "nicio fereastra");
    CHECK(pixel(bx + 150, by + 150) != (base_color(1) & 0xFFFFFF), "fundalul revine unde a fost B");
    close(A.cmd);
    close(A.rep);
    close(B.cmd);
    close(B.rep);

    yw_disconnect(ctl);
    ctl = NULL;
    kill(comp, SIGTERM);
    waitpid(comp, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "ycomp iese curat la SIGTERM: %d", stc);
    int fd = inputctl(INPUTCTL_GRAB, NULL);
    CHECK(fd >= 0, "dupa ycomp intrarea e eliberata");
    close(fd);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, SOCK);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    CHECK(connect(c, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno == ECONNREFUSED, "socket-ul ycomp a disparut");
    close(c);
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    test_shm();
    test_unix();
    test_input();
    test_rtstats();
    printf("comptest: kernel %d verificari, %d esecuri\n", checks, fails);
    test_compositor();
    printf("comptest: %d verificari, %d esecuri\n", checks, fails);
    if(fails){
        return 1;
    }
    printf("[OK] comptest\n");
    return 0;
}
