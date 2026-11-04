/* ywin (T10.2): client pentru compozitorul ycomp (vezi ywin.h si ywin_proto.h) */
#include "ywin.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifndef EPROTO
#define EPROTO 71
#endif
#define QMAX 256
#define REPLY_TIMEOUT_MS 10000

struct YwWindow {
    YwConn *c;
    uint32_t id;
    int w, h, x, y;
    uint32_t *pixels;
    size_t len;
    YgSurface surf;
    uint32_t frames;
    YwWindow *next;
};

struct YwConn {
    int fd;
    int sw, sh, id;
    uint8_t in[4096];
    int inlen;
    struct yw_msg q[QMAX];
    int qh, qn;
    unsigned serial;
    YwWindow *wins;
};

static long long now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static YwWindow *find_win(YwConn *c, uint32_t id){
    for(YwWindow *w = c->wins; w; w = w->next){
        if(w->id == id){
            return w;
        }
    }
    return NULL;
}

static int send_msg(YwConn *c, struct yw_msg *m){
    const uint8_t *p = (const uint8_t *)m;
    size_t done = 0;
    while(done < sizeof(*m)){
        ssize_t n = send(c->fd, p + done, sizeof(*m) - done, MSG_NOSIGNAL);
        if(n < 0){
            if(errno == EINTR){
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static void msg_init(struct yw_msg *m, uint32_t type, uint32_t win){
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->win = win;
}

/* Un mesaj complet din bufer; 1 daca exista */
static int take(YwConn *c, struct yw_msg *m){
    if(c->inlen < (int)sizeof(*m)){
        return 0;
    }
    memcpy(m, c->in, sizeof(*m));
    memmove(c->in, c->in + sizeof(*m), (size_t)c->inlen - sizeof(*m));
    c->inlen -= (int)sizeof(*m);
    if(m->type == YW_FRAME){
        YwWindow *w = find_win(c, m->win);
        if(w){
            w->frames++;
        }
    }
    return 1;
}

/* Urmatorul mesaj de pe socket (nu din coada): 1, 0 la expirare, -1 eroare / EOF */
static int recv_msg(YwConn *c, struct yw_msg *m, int timeout_ms){
    long long end = timeout_ms >= 0 ? now_ms() + timeout_ms : 0;
    for(;;){
        if(take(c, m)){
            return 1;
        }
        int t = -1;
        if(timeout_ms >= 0){
            long long left = end - now_ms();
            t = left > 0 ? (int)left : 0;
        }
        struct pollfd pf = {c->fd, POLLIN, 0};
        int r = poll(&pf, 1, t);
        if(r < 0){
            if(errno == EINTR){
                continue;
            }
            return -1;
        }
        if(r == 0){
            return 0;
        }
        ssize_t n = read(c->fd, c->in + c->inlen, sizeof(c->in) - (size_t)c->inlen);
        if(n < 0){
            if(errno == EINTR || errno == EAGAIN){
                continue;
            }
            return -1;
        }
        if(n == 0){
            errno = ECONNRESET;
            return -1;
        }
        c->inlen += (int)n;
    }
}

static void enqueue(YwConn *c, const struct yw_msg *m){
    if(c->qn == QMAX){
        c->qh = (c->qh + 1) % QMAX;     /* plin: se pierde cel mai vechi */
        c->qn--;
    }
    c->q[(c->qh + c->qn) % QMAX] = *m;
    c->qn++;
}

static int is_reply(uint32_t t){
    return t == YW_WELCOME || t == YW_CREATED || t == YW_ATTACHED || t == YW_ERROR || t == YW_REPLY;
}

/* Asteapta un raspuns (evenimentele intre timp intra in coada) */
static int wait_reply(YwConn *c, struct yw_msg *m){
    for(;;){
        int r = recv_msg(c, m, REPLY_TIMEOUT_MS);
        if(r <= 0){
            if(r == 0){
                errno = ETIMEDOUT;
            }
            return -1;
        }
        if(is_reply(m->type)){
            return 0;
        }
        enqueue(c, m);
    }
}

YwConn *yw_connect(const char *path){
    YwConn *c = calloc(1, sizeof(*c));
    if(!c){
        return NULL;
    }
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(c->fd < 0){
        free(c);
        return NULL;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path ? path : YW_SOCK_PATH, sizeof(sa.sun_path) - 1);
    if(connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0){
        int e = errno;
        close(c->fd);
        free(c);
        errno = e;
        return NULL;
    }
    struct yw_msg m;
    msg_init(&m, YW_HELLO, 0);
    m.v[0] = YW_PROTO_VERSION;
    if(send_msg(c, &m) < 0 || wait_reply(c, &m) < 0 || m.type != YW_WELCOME){
        int e = m.type == YW_ERROR ? m.v[0] : errno;
        close(c->fd);
        free(c);
        errno = e ? e : EPROTO;
        return NULL;
    }
    c->sw = m.v[0];
    c->sh = m.v[1];
    c->id = m.v[2];
    return c;
}

void yw_disconnect(YwConn *c){
    if(!c){
        return;
    }
    while(c->wins){
        yw_window_destroy(c->wins);
    }
    close(c->fd);
    free(c);
}

int yw_fd(const YwConn *c){
    return c->fd;
}

int yw_screen_width(const YwConn *c){
    return c->sw;
}

int yw_screen_height(const YwConn *c){
    return c->sh;
}

YwWindow *yw_window_new(YwConn *c, int w, int h, const char *title){
    return yw_window_new_role(c, w, h, title, YW_ROLE_NORMAL, 0, 0);
}

int yw_task_action(YwConn *c, uint32_t task, int action){
    struct yw_msg m;
    msg_init(&m, YW_TASK_ACT, 0);
    m.v[0] = (int32_t)task;
    m.v[1] = action;
    return send_msg(c, &m);
}

YwWindow *yw_window_new_role(YwConn *c, int w, int h, const char *title, int role, int x, int y){
    if(w <= 0 || h <= 0 || w > 4096 || h > 4096){
        errno = EINVAL;
        return NULL;
    }
    struct yw_msg m;
    msg_init(&m, YW_CREATE, 0);
    m.v[0] = w;
    m.v[1] = h;
    m.v[2] = role;
    m.v[3] = x;
    m.v[4] = y;
    strncpy(m.s, title ? title : "", sizeof(m.s) - 1);
    if(send_msg(c, &m) < 0 || wait_reply(c, &m) < 0){
        return NULL;
    }
    if(m.type != YW_CREATED){
        errno = m.type == YW_ERROR ? m.v[0] : EPROTO;
        return NULL;
    }
    YwWindow *win = calloc(1, sizeof(*win));
    if(!win){
        return NULL;
    }
    win->c = c;
    win->id = m.win;
    win->w = w;
    win->h = h;
    win->x = m.v[0];
    win->y = m.v[1];
    win->len = ((size_t)w * h * 4 + 4095) & ~(size_t)4095;

    char name[32];
    snprintf(name, sizeof(name), "/ywin-%d-%u", (int)getpid(), ++c->serial);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0644);
    if(fd < 0){
        goto fail;
    }
    if(ftruncate(fd, (off_t)win->len) < 0){
        goto fail_unlink;
    }
    void *p = mmap(NULL, win->len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(p == MAP_FAILED){
        goto fail_unlink;
    }
    win->pixels = p;
    yg_surface_wrap(&win->surf, win->pixels, w, h, w);
    msg_init(&m, YW_ATTACH, win->id);
    strncpy(m.s, name, sizeof(m.s) - 1);
    m.v[0] = w;
    m.v[1] = h;
    m.v[2] = w;
    m.v[3] = 0;
    win->next = c->wins;
    c->wins = win;
    if(send_msg(c, &m) < 0 || wait_reply(c, &m) < 0 || m.type != YW_ATTACHED){
        int e = m.type == YW_ERROR ? m.v[0] : errno;
        shm_unlink(name);
        close(fd);
        yw_window_destroy(win);
        errno = e ? e : EPROTO;
        return NULL;
    }
    /* Compozitorul are maparea lui: numele nu mai trebuie (obiectul traieste cat maparile) */
    shm_unlink(name);
    close(fd);
    return win;

fail_unlink: {
        int e = errno;
        shm_unlink(name);
        close(fd);
        errno = e;
    }
fail: {
        int e = errno;
        msg_init(&m, YW_DESTROY, win->id);
        send_msg(c, &m);
        free(win);
        errno = e;
        return NULL;
    }
}

void yw_window_destroy(YwWindow *w){
    YwConn *c = w->c;
    struct yw_msg m;
    msg_init(&m, YW_DESTROY, w->id);
    send_msg(c, &m);
    for(YwWindow **pp = &c->wins; *pp; pp = &(*pp)->next){
        if(*pp == w){
            *pp = w->next;
            break;
        }
    }
    if(w->pixels){
        munmap(w->pixels, w->len);
    }
    free(w);
}

uint32_t yw_window_id(const YwWindow *w){
    return w->id;
}

YgSurface *yw_window_surface(YwWindow *w){
    return &w->surf;
}

void yw_window_position(const YwWindow *w, int *x, int *y){
    if(x){
        *x = w->x;
    }
    if(y){
        *y = w->y;
    }
}

int yw_window_damage(YwWindow *w, int x, int y, int width, int height){
    struct yw_msg m;
    msg_init(&m, YW_DAMAGE, w->id);
    m.v[0] = x;
    m.v[1] = y;
    m.v[2] = width;
    m.v[3] = height;
    return send_msg(w->c, &m);
}

int yw_window_damage_all(YwWindow *w){
    return yw_window_damage(w, 0, 0, w->w, w->h);
}

int yw_window_set_title(YwWindow *w, const char *title){
    struct yw_msg m;
    msg_init(&m, YW_TITLE, w->id);
    strncpy(m.s, title, sizeof(m.s) - 1);
    return send_msg(w->c, &m);
}

int yw_window_move(YwWindow *w, int x, int y){
    struct yw_msg m;
    msg_init(&m, YW_MOVE, w->id);
    m.v[0] = x;
    m.v[1] = y;
    return send_msg(w->c, &m);
}

void yw_window_size(const YwWindow *w, int *width, int *height){
    if(width){
        *width = w->w;
    }
    if(height){
        *height = w->h;
    }
}

uint32_t yw_window_frames(const YwWindow *w){
    return w->frames;
}

int yw_window_wait_frame(YwWindow *w, int timeout_ms){
    YwConn *c = w->c;
    for(int i = 0; i < c->qn; i++){
        struct yw_msg *q = &c->q[(c->qh + i) % QMAX];
        if(q->type == YW_FRAME && q->win == w->id){
            for(int j = i; j + 1 < c->qn; j++){
                c->q[(c->qh + j) % QMAX] = c->q[(c->qh + j + 1) % QMAX];
            }
            c->qn--;
            return 0;
        }
    }
    long long end = timeout_ms >= 0 ? now_ms() + timeout_ms : 0;
    for(;;){
        int t = -1;
        if(timeout_ms >= 0){
            long long left = end - now_ms();
            t = left > 0 ? (int)left : 0;
        }
        struct yw_msg m;
        int r = recv_msg(c, &m, t);
        if(r <= 0){
            if(r == 0){
                errno = ETIMEDOUT;
            }
            return -1;
        }
        if(m.type == YW_FRAME && m.win == w->id){
            return 0;
        }
        enqueue(c, &m);
    }
}

/* YW_EV_RESIZE: bufer nou (continutul vechi se pastreaza in colt), atasat compozitorului; la eroare ramane cel vechi */
static int window_remap(YwWindow *w, int nw, int nh){
    YwConn *c = w->c;
    if(nw <= 0 || nh <= 0 || nw > 4096 || nh > 4096){
        return -1;
    }
    if(nw == w->w && nh == w->h){
        return 0;
    }
    size_t len = ((size_t)nw * nh * 4 + 4095) & ~(size_t)4095;
    char name[32];
    snprintf(name, sizeof(name), "/ywin-%d-%u", (int)getpid(), ++c->serial);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0644);
    if(fd < 0){
        return -1;
    }
    if(ftruncate(fd, (off_t)len) < 0){
        goto fail;
    }
    uint32_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(p == MAP_FAILED){
        goto fail;
    }
    int cw = nw < w->w ? nw : w->w, chh = nh < w->h ? nh : w->h;
    for(int y = 0; y < chh; y++){
        memcpy(p + (size_t)y * nw, w->pixels + (size_t)y * w->w, (size_t)cw * 4);
    }
    struct yw_msg m;
    msg_init(&m, YW_ATTACH, w->id);
    strncpy(m.s, name, sizeof(m.s) - 1);
    m.v[0] = nw;
    m.v[1] = nh;
    m.v[2] = nw;
    m.v[3] = 0;
    if(send_msg(c, &m) < 0 || wait_reply(c, &m) < 0 || m.type != YW_ATTACHED){
        munmap(p, len);
        goto fail;
    }
    shm_unlink(name);
    close(fd);
    munmap(w->pixels, w->len);
    w->pixels = p;
    w->len = len;
    w->w = nw;
    w->h = nh;
    yg_surface_wrap(&w->surf, p, nw, nh, nw);
    return 0;

fail:
    shm_unlink(name);
    close(fd);
    return -1;
}

static void resize_if_needed(YwConn *c, struct yw_msg *m){
    if(m->type != YW_EV_RESIZE){
        return;
    }
    YwWindow *w = find_win(c, m->win);
    if(w && window_remap(w, m->v[0], m->v[1]) < 0){
        m->v[0] = w->w;                 /* ramane marimea de care dispune clientul de fapt */
        m->v[1] = w->h;
    }
}

static void to_event(YwConn *c, const struct yw_msg *m, YwEvent *ev){
    memset(ev, 0, sizeof(*ev));
    ev->type = (int)m->type;
    ev->win = m->win;
    ev->window = find_win(c, m->win);
    switch(m->type){
    case YW_EV_KEY:
        ev->keycode = m->v[0];
        ev->ascii = m->v[1];
        ev->pressed = m->v[2];
        ev->modifiers = m->v[3];
        break;
    case YW_EV_MOUSE:
        ev->x = m->v[0];
        ev->y = m->v[1];
        ev->buttons = m->v[2];
        ev->wheel = m->v[3];
        ev->kind = m->v[4];
        break;
    case YW_EV_ENTER:
        ev->x = m->v[0];
        ev->y = m->v[1];
        break;
    case YW_EV_FOCUS:
        ev->focus = m->v[0];
        break;
    case YW_FRAME:
        ev->frame = (uint32_t)m->v[0];
        break;
    case YW_EV_RESIZE:
        ev->x = m->v[0];
        ev->y = m->v[1];
        break;
    case YW_EV_TASK:
        ev->task = (uint32_t)m->v[0];
        ev->task_flags = m->v[1];
        ev->task_ws = m->v[2];
        memcpy(ev->title, m->s, sizeof(ev->title));
        ev->title[sizeof(ev->title) - 1] = 0;
        break;
    default:
        break;
    }
}

int yw_pending(const YwConn *c){
    return c->qn + c->inlen / (int)sizeof(struct yw_msg);
}

int yw_next_event(YwConn *c, YwEvent *ev, int timeout_ms){
    struct yw_msg m;
    if(c->qn){
        m = c->q[c->qh];
        c->qh = (c->qh + 1) % QMAX;
        c->qn--;
        resize_if_needed(c, &m);
        to_event(c, &m, ev);
        return 1;
    }
    for(;;){
        int r = recv_msg(c, &m, timeout_ms);
        if(r <= 0){
            return r;
        }
        if(!is_reply(m.type)){
            resize_if_needed(c, &m);
            to_event(c, &m, ev);
            return 1;
        }
        /* raspuns intarziat la o cerere abandonata: se ignora */
    }
}

int yw_query(YwConn *c, int kind, int a, int b, int32_t out[6]){
    struct yw_msg m;
    msg_init(&m, YW_QUERY, 0);
    m.v[0] = kind;
    m.v[1] = a;
    m.v[2] = b;
    if(send_msg(c, &m) < 0 || wait_reply(c, &m) < 0 || m.type != YW_REPLY){
        return -1;
    }
    memcpy(out, m.v, sizeof(m.v));
    return 0;
}
