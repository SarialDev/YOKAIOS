/* ycomp (T10.2): compozitor de ferestre pe /dev/fb0.
 *   ycomp [-s socket] [-t dark|light] [-f fps] [-q] [-d] [-e "comanda"]...
 * Clientii se conecteaza pe un socket AF_UNIX (implicit /run/ycomp.sock, protocolul din ywin_proto.h), au
 * buferele in memorie partajata (shm_open) si anunta zonele schimbate. Compozitorul aduna dreptunghiurile murdare,
 * le compune intr-un back buffer si le copiaza pe ecran la granitele unui ceas de 60 FPS; fara schimbari doarme in
 * poll(-1) (zero treziri). Cand rt_stats() arata incarcare RT mare trece la 30 FPS si opreste animatiile.
 * -d porneste doua ferestre demo, -e o comanda (prin ysh -c) dupa ce socket-ul e gata. Ctrl+Alt+Backspace iese. */
#include "comp.h"
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

Comp C;

#define OPEN_MS   200
#define CLOSE_MS  150
#define RT_CHECK_MS 250
#define RT_CALM_CHECKS 4
#define RT_HIGH_PPM 500000u

static volatile sig_atomic_t stop_flag;
static int listen_fd = -1, input_fd = -1;
static int spawned;

long long now_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

long long now_ms(void){
    return now_us() / 1000;
}

static void on_signal(int sig){
    (void)sig;
    stop_flag = 1;
}

static void logmsg(const char *msg){
    if(!C.quiet){
        fprintf(stderr, "ycomp: %s\n", msg);
    }
}

/* ---- ritmul cadrelor ---- */

static long long period_us(void){
    return 1000000 / C.fps;
}

void request_frame(void){
    if(C.frame_scheduled){
        return;
    }
    long long now = now_us();
    long long p = period_us();
    long long k = (now - C.epoch_us + p - 1) / p;      /* urmatoarea granita a ceasului */
    if(k <= C.last_k){
        k = C.last_k + 1;
    }
    C.next_frame_us = C.epoch_us + k * p;
    C.frame_scheduled = 1;
}

static void set_rate(int fps, int anim){
    C.fps = fps;
    C.anim = anim;
    C.epoch_us = now_us();          /* ceasul nou porneste de acum */
    C.last_k = -1;
    if(C.frame_scheduled){
        C.frame_scheduled = 0;
        request_frame();
    }
}

static void rt_check(long long now){
    if(now - C.rt_checked_ms < RT_CHECK_MS){
        return;
    }
    C.rt_checked_ms = now;
    struct yk_rt_stats s;
    if(rt_stats(&s) < 0){
        return;
    }
    int high = s.load_ppm >= RT_HIGH_PPM || s.util_ppm >= RT_HIGH_PPM || s.deadline_misses > C.rt_misses;
    C.rt_misses = s.deadline_misses;
    C.rt_load_ppm = s.load_ppm;
    if(high){
        C.rt_calm = 0;
        if(C.fps != 30 || C.anim){
            C.degraded_count++;
            set_rate(C.fps_max < 30 ? C.fps_max : 30, 0);
            if(!C.quiet){
                fprintf(stderr, "ycomp: incarcare RT %u ppm: %d FPS, fara animatii\n", s.load_ppm, C.fps);
            }
        }
    } else if(++C.rt_calm >= RT_CALM_CHECKS && (C.fps != C.fps_max || !C.anim)){
        set_rate(C.fps_max, 1);
        logmsg("RT linistit: FPS normal, animatii pornite");
    }
}

/* ---- clienti ---- */

static void flush_client(Client *cl){
    while(cl->outlen){
        ssize_t n = send(cl->fd, cl->out, (size_t)cl->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if(n <= 0){
            return;                 /* EAGAIN: restul pleaca la POLLOUT; eroare: o vede citirea */
        }
        memmove(cl->out, cl->out + n, (size_t)(cl->outlen - n));
        cl->outlen -= (int)n;
    }
}

static void send_to(int ci, const struct yw_msg *m){
    Client *cl = &C.cl[ci];
    if(cl->fd < 0){
        return;
    }
    if(cl->outlen + (int)sizeof(*m) > (int)sizeof(cl->out)){
        cl->dropped++;              /* clientul nu citeste: evenimentul se pierde, conexiunea ramane */
        return;
    }
    memcpy(cl->out + cl->outlen, m, sizeof(*m));
    cl->outlen += (int)sizeof(*m);
    flush_client(cl);
}

static void reply(int ci, uint32_t type, uint32_t win, int v0, int v1, int v2, int v3, int v4, int v5){
    struct yw_msg m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.win = win;
    m.v[0] = v0;
    m.v[1] = v1;
    m.v[2] = v2;
    m.v[3] = v3;
    m.v[4] = v4;
    m.v[5] = v5;
    send_to(ci, &m);
}

void send_event(int widx, uint32_t type, int v0, int v1, int v2, int v3, int v4){
    Window *w = &C.win[widx];
    if(w->client < 0 || w->state == WS_CLOSING){
        return;
    }
    reply(w->client, type, w->id, v0, v1, v2, v3, v4, 0);
}

static int find_window(int ci, uint32_t id){
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].id == id && C.win[i].client == ci && C.win[i].state != WS_CLOSING){
            return i;
        }
    }
    return -1;
}

static int any_window_by_id(uint32_t id){
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].id == id){
            return i;
        }
    }
    return -1;
}

static void focus_top(void){
    for(int z = C.nz - 1; z >= 0; z--){
        if(C.win[C.zorder[z]].state != WS_CLOSING){
            focus_window(C.zorder[z]);
            return;
        }
    }
    focus_window(-1);
}

void window_remove(int idx){
    Window *w = &C.win[idx];
    damage_window(w);
    if(w->pixels){
        munmap(w->map, w->map_len);
    }
    int k = 0;
    for(int z = 0; z < C.nz; z++){
        if(C.zorder[z] != idx){
            C.zorder[k++] = C.zorder[z];
        }
    }
    C.nz = k;
    if(C.hover == idx) C.hover = -1;
    if(C.grab == idx) C.grab = -1;
    if(C.drag == idx) C.drag = -1;
    memset(w, 0, sizeof(*w));
    if(C.focus == idx){
        C.focus = -1;
        focus_top();
    }
}

/* Fereastra dispare (cu animatie daca sunt pornite) */
static void window_close(int idx){
    Window *w = &C.win[idx];
    if(C.focus == idx){
        C.focus = -1;
    }
    if(C.hover == idx) C.hover = -1;
    if(C.grab == idx) C.grab = -1;
    if(C.drag == idx) C.drag = -1;
    if(!C.anim){
        window_remove(idx);
        focus_top();
        return;
    }
    w->state = WS_CLOSING;
    w->anim_start = now_ms();
    focus_top();
    damage_window(w);
}

static void create_window(int ci, const struct yw_msg *m){
    int w = m->v[0], h = m->v[1];
    if(w <= 0 || h <= 0 || w > 4096 || h > 4096){
        reply(ci, YW_ERROR, 0, EINVAL, YW_CREATE, 0, 0, 0, 0);
        return;
    }
    int idx = -1;
    int count = 0;
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(!C.win[i].used){
            if(idx < 0) idx = i;
        } else {
            count++;
        }
    }
    if(idx < 0){
        reply(ci, YW_ERROR, 0, ENOSPC, YW_CREATE, 0, 0, 0, 0);
        return;
    }
    Window *win = &C.win[idx];
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->id = ++C.next_id;
    win->client = ci;
    win->w = w;
    win->h = h;
    memcpy(win->title, m->s, sizeof(win->title));
    win->title[sizeof(win->title) - 1] = 0;
    /* In cascada, cat incape pe ecran */
    int ow = w + 2 * BORDER, oh = h + TITLE_H + BORDER;
    int x = 40 + (count % 8) * 48, y = 40 + (count % 8) * 36;
    if(x + ow > C.sw) x = C.sw - ow > 0 ? C.sw - ow : 0;
    if(y + oh > C.sh) y = C.sh - oh > 0 ? C.sh - oh : 0;
    win->x = x;
    win->y = y;
    win->state = C.anim ? WS_OPENING : WS_NORMAL;
    win->opacity = C.anim ? 0 : 255;
    win->anim_start = now_ms();
    C.zorder[C.nz++] = idx;
    damage_window(win);
    YgRect c = window_content(win);
    reply(ci, YW_CREATED, win->id, c.x, c.y, 0, 0, 0, 0);
    focus_window(idx);
}

static void attach(int ci, const struct yw_msg *m){
    int idx = find_window(ci, m->win);
    if(idx < 0){
        reply(ci, YW_ERROR, m->win, ENOENT, YW_ATTACH, 0, 0, 0, 0);
        return;
    }
    Window *w = &C.win[idx];
    int bw = m->v[0], bh = m->v[1], stride = m->v[2], off = m->v[3];
    char name[33];
    memcpy(name, m->s, 32);
    name[32] = 0;
    if(bw != w->w || bh != w->h || stride < bw || off < 0 || (off & 3)){
        reply(ci, YW_ERROR, m->win, EINVAL, YW_ATTACH, 0, 0, 0, 0);
        return;
    }
    int fd = shm_open(name, O_RDONLY, 0);
    if(fd < 0){
        reply(ci, YW_ERROR, m->win, errno, YW_ATTACH, 0, 0, 0, 0);
        return;
    }
    struct stat st;
    size_t need = (size_t)off + (size_t)stride * bh * 4;
    if(fstat(fd, &st) < 0 || (size_t)st.st_size < need){
        close(fd);
        reply(ci, YW_ERROR, m->win, EINVAL, YW_ATTACH, 0, 0, 0, 0);
        return;
    }
    size_t len = ((size_t)st.st_size + 4095) & ~(size_t)4095;
    void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if(p == MAP_FAILED){
        reply(ci, YW_ERROR, m->win, errno, YW_ATTACH, 0, 0, 0, 0);
        return;
    }
    if(w->pixels){
        munmap(w->map, w->map_len);
    }
    w->map = p;
    w->pixels = (uint32_t *)((uint8_t *)p + off);
    w->map_len = len;
    w->stride = stride;
    damage(window_content(w));
    reply(ci, YW_ATTACHED, w->id, 0, 0, 0, 0, 0, 0);
}

static void query(int ci, const struct yw_msg *m){
    int32_t v[6] = {0, 0, 0, 0, 0, 0};
    switch(m->v[0]){
    case YW_Q_STATS: {
        int n = 0;
        for(int i = 0; i < MAX_WINDOWS; i++){
            n += C.win[i].used;
        }
        v[0] = (int32_t)C.frames;
        v[1] = (int32_t)C.wakeups;
        v[2] = C.fps;
        v[3] = C.anim;
        v[4] = n;
        v[5] = C.focus >= 0 ? (int32_t)C.win[C.focus].id : 0;
        break;
    }
    case YW_Q_STATS2:
        v[0] = C.cx;
        v[1] = C.cy;
        v[2] = (int32_t)C.rt_load_ppm;
        v[3] = (int32_t)C.degraded_count;
        v[4] = (int32_t)(C.pixels / 1000);
        v[5] = sched_getscheduler(0);
        break;
    case YW_Q_WIN: {
        int idx = any_window_by_id((uint32_t)m->v[1]);
        if(idx < 0){
            v[2] = -1;
            break;
        }
        YgRect c = window_content(&C.win[idx]);
        v[0] = c.x;
        v[1] = c.y;
        v[2] = c.w;
        v[3] = c.h;
        v[4] = (int32_t)C.win[idx].damage_msgs;
        v[5] = (int32_t)C.win[idx].frames_sent;
        break;
    }
    case YW_Q_PIXEL:
        v[0] = (int32_t)back_pixel(m->v[1], m->v[2]);
        break;
    default:
        reply(ci, YW_ERROR, 0, EINVAL, YW_QUERY, 0, 0, 0, 0);
        return;
    }
    reply(ci, YW_REPLY, 0, v[0], v[1], v[2], v[3], v[4], v[5]);
}

static void handle(int ci, const struct yw_msg *m){
    int idx;
    switch(m->type){
    case YW_HELLO:
        reply(ci, YW_WELCOME, 0, C.sw, C.sh, ci + 1, 0, 0, 0);
        break;
    case YW_CREATE:
        create_window(ci, m);
        break;
    case YW_ATTACH:
        attach(ci, m);
        break;
    case YW_DAMAGE:
        idx = find_window(ci, m->win);
        if(idx >= 0){
            Window *w = &C.win[idx];
            YgRect c = window_content(w);
            YgRect r = yg_rect_intersect(yg_rect(c.x + m->v[0], c.y + m->v[1], m->v[2], m->v[3]), c);
            w->damage_msgs++;
            w->pending_frame = 1;           /* YW_FRAME si pentru o zona goala / acoperita */
            if(!yg_rect_empty(r)){
                damage(r);
            } else {
                request_frame();
            }
        }
        break;
    case YW_TITLE:
        idx = find_window(ci, m->win);
        if(idx >= 0){
            memcpy(C.win[idx].title, m->s, sizeof(C.win[idx].title));
            C.win[idx].title[sizeof(C.win[idx].title) - 1] = 0;
            damage(window_title(&C.win[idx]));
        }
        break;
    case YW_MOVE:
        idx = find_window(ci, m->win);
        if(idx >= 0){
            damage_window(&C.win[idx]);
            C.win[idx].x = m->v[0];
            C.win[idx].y = m->v[1];
            damage_window(&C.win[idx]);
        }
        break;
    case YW_DESTROY:
        idx = find_window(ci, m->win);
        if(idx >= 0){
            window_close(idx);
        }
        break;
    case YW_QUERY:
        query(ci, m);
        break;
    default:
        reply(ci, YW_ERROR, m->win, EINVAL, m->type, 0, 0, 0, 0);
        break;
    }
}

static void drop_client(int ci){
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].client == ci && C.win[i].state != WS_CLOSING){
            window_close(i);
        }
    }
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].client == ci){
            C.win[i].client = -1;
        }
    }
    close(C.cl[ci].fd);
    C.cl[ci].fd = -1;
    C.cl[ci].inlen = C.cl[ci].outlen = 0;
}

static void read_client(int ci){
    Client *cl = &C.cl[ci];
    for(;;){
        ssize_t n = read(cl->fd, cl->in + cl->inlen, sizeof(cl->in) - (size_t)cl->inlen);
        if(n < 0 && (errno == EAGAIN || errno == EINTR)){
            return;
        }
        if(n <= 0){
            drop_client(ci);
            return;
        }
        cl->inlen += (int)n;
        int off = 0;
        while(cl->inlen - off >= (int)sizeof(struct yw_msg)){
            struct yw_msg m;
            memcpy(&m, cl->in + off, sizeof(m));
            off += (int)sizeof(m);
            handle(ci, &m);
            if(cl->fd < 0){
                return;
            }
        }
        memmove(cl->in, cl->in + off, (size_t)(cl->inlen - off));
        cl->inlen -= off;
    }
}

static void accept_clients(void){
    for(;;){
        int fd = accept(listen_fd, NULL, NULL);
        if(fd < 0){
            return;
        }
        int ci = -1;
        for(int i = 0; i < MAX_CLIENTS; i++){
            if(C.cl[i].fd < 0){
                ci = i;
                break;
            }
        }
        if(ci < 0){
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        memset(&C.cl[ci], 0, sizeof(C.cl[ci]));
        C.cl[ci].fd = fd;
    }
}

static void read_input(void){
    struct yk_input_event ev[32];
    ssize_t n = read(input_fd, ev, sizeof(ev));
    for(ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++){
        input_event(&ev[i]);
    }
}

/* ---- un cadru ---- */

static int animating(void){
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].state != WS_NORMAL){
            return 1;
        }
    }
    return 0;
}

static void step_animations(long long now){
    for(int i = 0; i < MAX_WINDOWS; i++){
        Window *w = &C.win[i];
        if(!w->used || w->state == WS_NORMAL){
            continue;
        }
        long long t = now - w->anim_start;
        if(w->state == WS_OPENING){
            if(!C.anim || t >= OPEN_MS){
                w->opacity = 255;
                w->state = WS_NORMAL;
            } else {
                w->opacity = (uint8_t)(t * 255 / OPEN_MS);
            }
            damage_window(w);
        } else if(!C.anim || t >= CLOSE_MS){
            window_remove(i);
        } else {
            w->opacity = (uint8_t)(255 - t * 255 / CLOSE_MS);
            damage_window(w);
        }
    }
}

static void frame(void){
    long long now = now_ms();
    C.frame_scheduled = 0;
    C.last_k = (C.next_frame_us - C.epoch_us) / period_us();
    rt_check(now);
    step_animations(now);
    if(C.ndirty){
        compose_and_flip();
    }
    for(int i = 0; i < MAX_WINDOWS; i++){
        Window *w = &C.win[i];
        if(w->used && w->pending_frame){
            w->pending_frame = 0;
            w->frames_sent++;
            send_event(i, YW_FRAME, (int)C.frames, 0, 0, 0, 0);
        }
    }
    if(animating()){
        request_frame();
    }
}

static void spawn(const char *cmd){
    pid_t pid = fork();
    if(pid == 0){
        char *const av[] = {"ysh", "-c", (char *)cmd, NULL};
        execv("/bin/ysh", av);
        _exit(127);
    }
    if(pid > 0){
        spawned++;
    }
}

static int usage(void){
    printf("uz: ycomp [-s socket] [-t dark|light] [-f fps] [-q] [-d] [-e comanda]...\n");
    return 2;
}

int main(int argc, char **argv){
    const char *path = YW_SOCK_PATH;
    const char *theme = NULL;
    const char *cmds[8];
    int ncmds = 0;
    C.fps_max = 60;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            usage();
            return 0;
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            path = argv[++i];
        } else if(!strcmp(argv[i], "-t") && i + 1 < argc){
            theme = argv[++i];
        } else if(!strcmp(argv[i], "-f") && i + 1 < argc){
            C.fps_max = atoi(argv[++i]);
            if(C.fps_max < 1 || C.fps_max > 240){
                return usage();
            }
        } else if(!strcmp(argv[i], "-q")){
            C.quiet = 1;
        } else if(!strcmp(argv[i], "-d") && ncmds + 2 <= 8){
            cmds[ncmds++] = "ywdemo -m clock -t Ceas";
            cmds[ncmds++] = "ywdemo -m balls -t Bile -p 16";
        } else if(!strcmp(argv[i], "-e") && i + 1 < argc && ncmds < 8){
            cmds[ncmds++] = argv[++i];
        } else {
            return usage();
        }
    }

    /* Prioritate NORMAL chiar daca parintele e RT: compozitorul nu are voie sa intarzie clasa RT */
    struct sched_param sp = {0};
    sched_setscheduler(0, SCHED_OTHER, &sp);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);
    if(listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&sun, sizeof(sun)) < 0 || listen(listen_fd, 8) < 0){
        fprintf(stderr, "ycomp: %s: %s\n", path, strerror(errno));
        return 1;
    }
    for(int i = 0; i < MAX_CLIENTS; i++){
        C.cl[i].fd = -1;
    }
    C.focus = C.hover = C.grab = C.drag = -1;
    C.fps = C.fps_max;
    C.anim = 1;
    C.epoch_us = now_us();
    C.last_k = -1;
    input_fd = inputctl(INPUTCTL_GRAB, NULL);
    if(input_fd < 0){
        fprintf(stderr, "ycomp: fara input: %s\n", strerror(errno));
    }
    int err = render_init(theme);
    if(err){
        fprintf(stderr, "ycomp: /dev/fb0: %s\n", strerror(-err));
        return 1;
    }
    for(int i = 0; i < ncmds; i++){
        spawn(cmds[i]);
    }
    if(!C.quiet){
        fprintf(stderr, "ycomp: %dx%d, %s, %d FPS\n", C.sw, C.sh, path, C.fps);
    }

    while(!stop_flag){
        struct pollfd pf[2 + MAX_CLIENTS];
        int map[2 + MAX_CLIENTS];
        int n = 0;
        pf[n].fd = listen_fd;
        pf[n].events = POLLIN;
        map[n++] = -1;
        if(input_fd >= 0){
            pf[n].fd = input_fd;
            pf[n].events = POLLIN;
            map[n++] = -2;
        }
        for(int i = 0; i < MAX_CLIENTS; i++){
            if(C.cl[i].fd >= 0){
                pf[n].fd = C.cl[i].fd;
                pf[n].events = (short)(POLLIN | (C.cl[i].outlen ? POLLOUT : 0));
                map[n++] = i;
            }
        }
        int timeout = -1;
        if(C.frame_scheduled){
            long long left = C.next_frame_us - now_us();
            timeout = left > 0 ? (int)((left + 999) / 1000) : 0;
        }
        int r = poll(pf, (nfds_t)n, timeout);
        C.wakeups++;
        if(stop_flag){
            break;
        }
        if(r > 0){
            for(int i = 0; i < n; i++){
                if(!pf[i].revents){
                    continue;
                }
                if(map[i] == -1){
                    accept_clients();
                } else if(map[i] == -2){
                    read_input();
                } else if(C.cl[map[i]].fd >= 0){
                    if(pf[i].revents & POLLOUT){
                        flush_client(&C.cl[map[i]]);
                    }
                    if(pf[i].revents & (POLLIN | POLLHUP | POLLERR)){
                        read_client(map[i]);
                    }
                }
            }
        }
        if(C.frame_scheduled && now_us() >= C.next_frame_us){
            frame();
        }
        if(spawned){
            while(waitpid(-1, NULL, WNOHANG) > 0){
                spawned--;
            }
        }
    }

    for(int i = 0; i < MAX_CLIENTS; i++){
        if(C.cl[i].fd >= 0){
            close(C.cl[i].fd);
        }
    }
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].pixels){
            munmap(C.win[i].map, C.win[i].map_len);
        }
    }
    render_fini();
    if(input_fd >= 0){
        close(input_fd);
    }
    close(listen_fd);
    if(!C.quiet){
        fprintf(stderr, "ycomp: gata (%u cadre)\n", C.frames);
    }
    return 0;
}
