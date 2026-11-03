// Socket-uri BSD (T9.3): File-uri peste socket-urile lwIP. Fisierul include antetele lwIP (cu E* si constantele lor) si
// de aceea NU include uapi/syscall_nr.h / uapi/net.h: valorile de pe Linux sunt repetate mai jos cu prefix K_, iar
// codurile de eroare vin din lwIP (aceleasi numere ca pe Linux).
#include "net/socket.h"
#include "net/lwip_port.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "lwip/sockets.h"

bool signal_pending();                      // proc/signal.h (nu il includem: aduce syscall_nr.h)

namespace {

// Valorile Linux (uapi/net.h)
const int K_SOCK_STREAM = 1;
const int K_SOCK_DGRAM = 2;
const int K_SOCK_RAW = 3;
const int K_IPPROTO_ICMP = 1;
const int K_SOCK_NONBLOCK = 04000;
const int K_SOL_SOCKET = 1;
const int K_SO_REUSEADDR = 2;
const int K_SO_TYPE = 3;
const int K_SO_ERROR = 4;
const int K_SO_BROADCAST = 6;
const int K_SO_SNDBUF = 7;
const int K_SO_RCVBUF = 8;
const int K_SO_KEEPALIVE = 9;
const int K_SO_RCVTIMEO = 20;
const int K_SO_SNDTIMEO = 21;
const int K_IPPROTO_TCP = 6;
const int K_TCP_NODELAY = 1;
const int K_MSG_PEEK = 0x02;
const int K_MSG_DONTWAIT = 0x40;
const uint32_t K_O_NONBLOCK = 04000;
const uint32_t K_POLLIN = 0x01, K_POLLOUT = 0x04, K_POLLERR = 0x08, K_POLLHUP = 0x10, K_POLLNVAL = 0x20;
const int K_EPROTONOSUPPORT = 93;
const int K_ENOPROTOOPT = 92;
const int K_EMSGSIZE = 90;

const int MAX_POLL = 32;                    // PROC_MAX_FD
const uint64_t SLICE_MS = 20;               // o asteptare lunga se face in felii, ca un semnal sa o poata intrerupe

struct Sock {
    int lfd;                                // descriptorul lwIP
    int type;                               // K_SOCK_STREAM / K_SOCK_DGRAM
    uint32_t rcvto_ms;                      // 0 = fara limita
    uint32_t sndto_ms;
    bool connected;
    bool listening;
};

Sock* sk(const File* f){
    return (Sock*)f->priv;
}

bool nonblocking(const File* f, int flags){
    return (f->flags & K_O_NONBLOCK) || (flags & K_MSG_DONTWAIT);
}

uint64_t deadline_for(uint32_t ms){
    return ms ? monotonic_ns() + (uint64_t)ms * 1000000UL : 0;
}

// Asteapta pana la o felie de timp (sau pana apare evenimentul ev lwIP): 0 = reia incercarea, -EINTR, -EAGAIN la expirare
int wait_ready(Sock* s, short ev, uint64_t deadline){
    if(signal_pending()){
        return -EINTR;
    }
    uint64_t ms = SLICE_MS;
    if(deadline){
        uint64_t now = monotonic_ns();
        if(now >= deadline){
            return -EAGAIN;
        }
        uint64_t rem = (deadline - now + 999999UL) / 1000000UL;
        if(rem < ms){
            ms = rem;
        }
    }
    struct pollfd p;
    p.fd = s->lfd;
    p.events = ev;
    p.revents = 0;
    lwip_poll(&p, 1, (int)ms);
    return 0;
}

void to_lwip(const KSockAddr& a, struct sockaddr_in& sa){
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = a.port;
    sa.sin_addr.s_addr = a.ip;
}

void from_lwip(const struct sockaddr_in& sa, KSockAddr& a){
    a.port = sa.sin_port;
    a.ip = sa.sin_addr.s_addr;
}

short lwip_events(uint32_t ev){
    return (short)(((ev & K_POLLIN) ? POLLIN : 0) | ((ev & K_POLLOUT) ? POLLOUT : 0));
}

uint32_t linux_events(short rev){
    return ((rev & POLLIN) ? K_POLLIN : 0) | ((rev & POLLOUT) ? K_POLLOUT : 0) | ((rev & POLLERR) ? K_POLLERR : 0) |
           ((rev & POLLHUP) ? K_POLLHUP : 0) | ((rev & POLLNVAL) ? K_POLLNVAL : 0);
}

int64_t sock_read(File* f, void* buf, uint64_t len, bool nonblock){
    bool hf;
    return sock_recvfrom(f, buf, len, nonblock ? K_MSG_DONTWAIT : 0, nullptr, &hf);
}

int64_t sock_write(File* f, const void* buf, uint64_t len, bool nonblock){
    return sock_sendto(f, buf, len, nonblock ? K_MSG_DONTWAIT : 0, nullptr);
}

void sock_close(File* f){
    Sock* s = sk(f);
    lwip_close(s->lfd);
    kfree(s);
}

uint32_t sock_poll_op(File* f){
    return sock_poll_now(f);
}

const FileOps socket_ops = {sock_read, sock_write, sock_close, nullptr, nullptr, sock_poll_op};

// File nou peste socket-ul lwIP lfd (preia descriptorul); la lipsa de memorie il inchide
int wrap(int lfd, int type, bool nonblock, File*& out){
    Sock* s = (Sock*)kmalloc(sizeof(Sock));
    File* f = s ? file_alloc(&socket_ops, s) : nullptr;
    if(!f){
        kfree(s);
        lwip_close(lfd);
        return -ENOMEM;
    }
    s->lfd = lfd;
    s->type = type;
    s->rcvto_ms = s->sndto_ms = 0;
    s->connected = s->listening = false;
    f->flags = nonblock ? K_O_NONBLOCK : 0;
    // lwIP ruleaza mereu neblocant; blocarea o face acest strat
    lwip_fcntl(lfd, F_SETFL, O_NONBLOCK);
    out = f;
    return 0;
}

}

bool sock_is(const File* f){
    return f && f->ops == &socket_ops;
}

bool sock_is_stream(const File* f){
    return sk(f)->type == K_SOCK_STREAM;
}

int sock_create(int type, int proto, File*& out){
    bool nb = type & K_SOCK_NONBLOCK;
    int t = type & ~K_SOCK_NONBLOCK;
    if(t != K_SOCK_STREAM && t != K_SOCK_DGRAM && !(t == K_SOCK_RAW && proto == K_IPPROTO_ICMP)){
        return -K_EPROTONOSUPPORT;
    }
    if(!net_ready()){
        return -ENETDOWN;
    }
    int lfd = lwip_socket(AF_INET, t, proto);
    if(lfd < 0){
        return -errno;
    }
    return wrap(lfd, t, nb, out);
}

int sock_bind(File* f, const KSockAddr& a){
    struct sockaddr_in sa;
    to_lwip(a, sa);
    return lwip_bind(sk(f)->lfd, (struct sockaddr*)&sa, sizeof(sa)) ? -errno : 0;
}

int sock_listen(File* f, int backlog){
    Sock* s = sk(f);
    if(lwip_listen(s->lfd, backlog < 1 ? 1 : backlog)){
        return -errno;
    }
    s->listening = true;
    return 0;
}

int sock_accept(File* f, File*& out, KSockAddr& peer){
    Sock* s = sk(f);
    if(s->type != K_SOCK_STREAM){
        return -EOPNOTSUPP;
    }
    bool nb = nonblocking(f, 0);
    uint64_t deadline = deadline_for(s->rcvto_ms);
    for(;;){
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int nfd = lwip_accept(s->lfd, (struct sockaddr*)&sa, &sl);
        if(nfd >= 0){
            from_lwip(sa, peer);
            int r = wrap(nfd, K_SOCK_STREAM, false, out);
            if(!r){
                sk(out)->connected = true;
            }
            return r;
        }
        int e = errno;
        if(e != EWOULDBLOCK){
            return -e;
        }
        if(nb){
            return -EAGAIN;
        }
        int w = wait_ready(s, POLLIN, deadline);
        if(w){
            return w;
        }
    }
}

int sock_connect(File* f, const KSockAddr& a){
    Sock* s = sk(f);
    struct sockaddr_in sa;
    to_lwip(a, sa);
    if(lwip_connect(s->lfd, (struct sockaddr*)&sa, sizeof(sa)) == 0){
        s->connected = true;
        return 0;
    }
    int e = errno;
    if(e != EINPROGRESS){
        return -e;
    }
    if(nonblocking(f, 0)){
        return -EINPROGRESS;
    }
    uint64_t deadline = deadline_for(s->sndto_ms);
    for(;;){
        int w = wait_ready(s, POLLOUT, deadline);
        if(w == -EAGAIN){
            return -ETIMEDOUT;
        }
        if(w){
            return w;
        }
        struct pollfd p;
        p.fd = s->lfd;
        p.events = POLLOUT;
        p.revents = 0;
        if(lwip_poll(&p, 1, 0) <= 0){
            continue;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        if(lwip_getsockopt(s->lfd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err){
            return -err;
        }
        s->connected = true;
        return 0;
    }
}

int64_t sock_sendto(File* f, const void* buf, uint64_t len, int flags, const KSockAddr* to){
    Sock* s = sk(f);
    bool nb = nonblocking(f, flags);
    uint64_t deadline = deadline_for(s->sndto_ms);
    const uint8_t* p = (const uint8_t*)buf;
    if(s->type != K_SOCK_STREAM){                 // DGRAM si RAW
        if(len > SOCK_IO_MAX * 2){
            return -K_EMSGSIZE;
        }
        struct sockaddr_in sa;
        if(to){
            to_lwip(*to, sa);
        }
        for(;;){
            int r = lwip_sendto(s->lfd, p, len, 0, to ? (struct sockaddr*)&sa : nullptr, to ? sizeof(sa) : 0);
            if(r >= 0){
                return r;
            }
            int e = errno;
            if(e != EWOULDBLOCK && e != ENOMEM && e != ENOBUFS){
                return -e;
            }
            if(nb){
                return -EAGAIN;
            }
            int w = wait_ready(s, POLLOUT, deadline);
            if(w){
                return w;
            }
        }
    }
    uint64_t done = 0;
    while(done < len){
        int r = lwip_send(s->lfd, p + done, len - done, 0);
        if(r > 0){
            done += (uint64_t)r;
            continue;
        }
        int e = r < 0 ? errno : EIO;
        if(e == EWOULDBLOCK || e == ENOMEM || e == ENOBUFS){
            if(nb){
                return done ? (int64_t)done : -EAGAIN;
            }
            int w = wait_ready(s, POLLOUT, deadline);
            if(w){
                return done ? (int64_t)done : w;
            }
            continue;
        }
        if(e == ENOTCONN && s->connected){
            e = EPIPE;                      // partenerul a inchis / resetat: write() trimite SIGPIPE
        }
        return done ? (int64_t)done : -e;
    }
    return (int64_t)done;
}

int64_t sock_recvfrom(File* f, void* buf, uint64_t len, int flags, KSockAddr* from, bool* has_from){
    Sock* s = sk(f);
    bool nb = nonblocking(f, flags);
    uint64_t deadline = deadline_for(s->rcvto_ms);
    int lf = (flags & K_MSG_PEEK) ? MSG_PEEK : 0;
    *has_from = false;
    for(;;){
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int r = lwip_recvfrom(s->lfd, buf, len, lf, (struct sockaddr*)&sa, &sl);
        if(r >= 0){
            if(from && s->type != K_SOCK_STREAM && sl >= sizeof(sa)){
                from_lwip(sa, *from);
                *has_from = true;
            }
            return r;
        }
        int e = errno;
        if(e != EWOULDBLOCK){
            return -e;
        }
        if(nb){
            return -EAGAIN;
        }
        int w = wait_ready(s, POLLIN, deadline);
        if(w){
            return w;
        }
    }
}

int sock_shutdown(File* f, int how){
    return lwip_shutdown(sk(f)->lfd, how) ? -errno : 0;
}

int sock_setopt(File* f, int level, int opt, const void* val, uint32_t len){
    Sock* s = sk(f);
    if(len < sizeof(int) && !(level == K_SOL_SOCKET && (opt == K_SO_RCVTIMEO || opt == K_SO_SNDTIMEO))){
        return -EINVAL;
    }
    int v = 0;
    if(len >= sizeof(int)){
        memcpy(&v, val, sizeof(v));
    }
    int lopt;
    if(level == K_SOL_SOCKET){
        switch(opt){
        case K_SO_RCVTIMEO:
        case K_SO_SNDTIMEO: {
            struct { long sec; long usec; } tv;
            if(len < sizeof(tv)){
                return -EINVAL;
            }
            memcpy(&tv, val, sizeof(tv));
            if(tv.sec < 0 || tv.usec < 0 || tv.usec >= 1000000){
                return -EINVAL;
            }
            uint64_t ms = (uint64_t)tv.sec * 1000 + (uint64_t)(tv.usec + 999) / 1000;
            if(ms > 0xFFFFFFFFUL){
                ms = 0xFFFFFFFFUL;
            }
            (opt == K_SO_RCVTIMEO ? s->rcvto_ms : s->sndto_ms) = (uint32_t)ms;
            return 0;
        }
        case K_SO_SNDBUF:
            return 0;                       // dimensiunea o fixeaza lwipopts.h
        case K_SO_REUSEADDR: lopt = SO_REUSEADDR; break;
        case K_SO_KEEPALIVE: lopt = SO_KEEPALIVE; break;
        case K_SO_BROADCAST: lopt = SO_BROADCAST; break;
        case K_SO_RCVBUF: lopt = SO_RCVBUF; break;
        default: return -K_ENOPROTOOPT;
        }
        return lwip_setsockopt(s->lfd, SOL_SOCKET, lopt, &v, sizeof(v)) ? -errno : 0;
    }
    if(level == K_IPPROTO_TCP && opt == K_TCP_NODELAY){
        return lwip_setsockopt(s->lfd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) ? -errno : 0;
    }
    return -K_ENOPROTOOPT;
}

int sock_getopt(File* f, int level, int opt, void* val, uint32_t* len){
    Sock* s = sk(f);
    int v = 0;
    if(level == K_SOL_SOCKET){
        int lopt;
        switch(opt){
        case K_SO_RCVTIMEO:
        case K_SO_SNDTIMEO: {
            uint32_t ms = opt == K_SO_RCVTIMEO ? s->rcvto_ms : s->sndto_ms;
            struct { long sec; long usec; } tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
            if(*len > sizeof(tv)){
                *len = sizeof(tv);
            }
            memcpy(val, &tv, *len);
            return 0;
        }
        case K_SO_TYPE: v = s->type; goto out;
        case K_SO_SNDBUF: v = TCP_SND_BUF; goto out;
        case K_SO_ERROR: lopt = SO_ERROR; break;
        case K_SO_REUSEADDR: lopt = SO_REUSEADDR; break;
        case K_SO_KEEPALIVE: lopt = SO_KEEPALIVE; break;
        case K_SO_BROADCAST: lopt = SO_BROADCAST; break;
        case K_SO_RCVBUF: lopt = SO_RCVBUF; break;
        default: return -K_ENOPROTOOPT;
        }
        socklen_t l = sizeof(v);
        if(lwip_getsockopt(s->lfd, SOL_SOCKET, lopt, &v, &l)){
            return -errno;
        }
    } else if(level == K_IPPROTO_TCP && opt == K_TCP_NODELAY){
        socklen_t l = sizeof(v);
        if(lwip_getsockopt(s->lfd, IPPROTO_TCP, TCP_NODELAY, &v, &l)){
            return -errno;
        }
    } else {
        return -K_ENOPROTOOPT;
    }
out:
    if(*len > sizeof(v)){
        *len = sizeof(v);
    }
    memcpy(val, &v, *len);
    return 0;
}

int sock_name(File* f, bool peer, KSockAddr& out){
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int r = peer ? lwip_getpeername(sk(f)->lfd, (struct sockaddr*)&sa, &sl) : lwip_getsockname(sk(f)->lfd, (struct sockaddr*)&sa, &sl);
    if(r){
        return -errno;
    }
    from_lwip(sa, out);
    return 0;
}

int sock_poll_wait(File* const* fs, const uint32_t* ev, uint32_t* rev, int n, int ms){
    struct pollfd p[MAX_POLL];
    if(n > MAX_POLL){
        n = MAX_POLL;
    }
    for(int i = 0; i < n; i++){
        p[i].fd = sk(fs[i])->lfd;
        p[i].events = lwip_events(ev[i]);
        p[i].revents = 0;
    }
    int r = n ? lwip_poll(p, (nfds_t)n, ms) : 0;
    int count = 0;
    for(int i = 0; i < n; i++){
        rev[i] = linux_events(p[i].revents);
        count += rev[i] != 0;
    }
    (void)r;
    return count;
}

uint32_t sock_poll_now(File* f){
    uint32_t ev = K_POLLIN | K_POLLOUT, rev = 0;
    sock_poll_wait(&f, &ev, &rev, 1, 0);
    return rev;
}
