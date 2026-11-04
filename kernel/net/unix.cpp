// Socket-uri AF_UNIX SOCK_STREAM (T10.2). Toata starea e sub un singur lock (ulock); fiecare socket are o coada de
// asteptare comuna pentru toate evenimentele lui (date sosite, loc eliberat la partener, conexiune in backlog,
// partener inchis): cine asteapta reverifica conditia. Orice schimbare cheama si poll_notify() pentru poll/select.
#include "net/unix.h"
#include "uapi/net.h"
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"
#include "sync/pollwait.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/new.h"

namespace {

enum class St : uint8_t { New, Listen, Conn };

const int BACKLOG_MAX = 16;

struct USock {
    uint32_t refs;              // File-ul + legatura de la partener + intrarea din backlog
    St state;
    bool file_closed;           // File-ul lui a fost inchis (nu mai citeste nimeni)
    bool shut_rd, shut_wr;      // shutdown local
    bool peer_gone;             // partenerul a inchis (dupa datele ramase: EOF / EPIPE)
    bool bound;
    USock* peer;
    uint8_t* buf;               // ce primeste acest socket
    uint32_t head, count;
    char name[UNIX_PATH_MAX];
    uint32_t name_len;
    USock* backlog[BACKLOG_MAX];
    int nback, maxback;
    USock* next_bound;
    WaitQueue wq;
};

Spinlock ulock("unix");
USock* bound_list = nullptr;
uint64_t live = 0;

USock* sock_of(File* f){
    return (USock*)f->priv;
}

// Cu ulock luat
void sget(USock* s){
    s->refs++;
}

// Cu ulock luat; la ultima referinta se elibereaza (bufferul se elibereaza dupa lock, de apelant: aici direct)
void sput(USock* s){
    if(--s->refs){
        return;
    }
    kfree(s->buf);
    s->~USock();
    kfree(s);
    live--;
}

USock* salloc(){
    void* mem = kzalloc(sizeof(USock));
    if(!mem){
        return nullptr;
    }
    USock* s = new(mem) USock();
    s->buf = (uint8_t*)kmalloc(UNIX_BUF);
    if(!s->buf){
        s->~USock();
        kfree(s);
        return nullptr;
    }
    s->refs = 1;
    s->state = St::New;
    uint64_t f = ulock.lock_irqsave();
    live++;
    ulock.unlock_irqrestore(f);
    return s;
}

void wake(USock* s){
    if(s){
        s->wq.wake_all();
    }
}

// Cu ulock luat
USock* find_bound(const char* name, uint32_t len){
    for(USock* s = bound_list; s; s = s->next_bound){
        if(s->name_len == len && !memcmp(s->name, name, len)){
            return s;
        }
    }
    return nullptr;
}

void unbind(USock* s){
    if(!s->bound){
        return;
    }
    USock** pp = &bound_list;
    while(*pp && *pp != s){
        pp = &(*pp)->next_bound;
    }
    if(*pp){
        *pp = s->next_bound;
    }
    s->bound = false;
}

// Cu ulock luat: rupe legatura s <-> partener (partenerul vede peer_gone)
void disconnect(USock* s){
    USock* p = s->peer;
    if(!p){
        return;
    }
    s->peer = nullptr;
    p->peer = nullptr;
    p->peer_gone = true;
    s->peer_gone = true;
    wake(p);
    sput(p);                    // legatura s -> p
    sput(s);                    // legatura p -> s
}

void pair_up(USock* a, USock* b){
    a->peer = b;
    b->peer = a;
    sget(a);
    sget(b);
    a->state = b->state = St::Conn;
}

uint32_t space_in(const USock* s){
    return UNIX_BUF - s->count;
}

void ring_put(USock* s, const uint8_t* src, uint32_t n){
    uint32_t tail = (s->head + s->count) % UNIX_BUF;
    uint32_t first = n < UNIX_BUF - tail ? n : UNIX_BUF - tail;
    memcpy(s->buf + tail, src, first);
    memcpy(s->buf, src + first, n - first);
    s->count += n;
}

void ring_get(USock* s, uint8_t* dst, uint32_t n, bool peek){
    uint32_t first = n < UNIX_BUF - s->head ? n : UNIX_BUF - s->head;
    memcpy(dst, s->buf + s->head, first);
    memcpy(dst + first, s->buf, n - first);
    if(!peek){
        s->head = (s->head + n) % UNIX_BUF;
        s->count -= n;
    }
}

int64_t f_read(File* f, void* buf, uint64_t len, bool nonblock){
    return unix_recv(f, buf, len, nonblock ? MSG_DONTWAIT : 0);
}

int64_t f_write(File* f, const void* buf, uint64_t len, bool nonblock){
    return unix_send(f, buf, len, nonblock ? MSG_DONTWAIT : 0);
}

void f_close(File* f){
    USock* s = sock_of(f);
    uint64_t fl = ulock.lock_irqsave();
    s->file_closed = true;
    unbind(s);
    // Conexiunile neacceptate se inchid (clientii vad EOF / EPIPE)
    for(int i = 0; i < s->nback; i++){
        USock* c = s->backlog[i];
        c->file_closed = true;
        disconnect(c);
        sput(c);
    }
    s->nback = 0;
    disconnect(s);
    wake(s);
    sput(s);
    ulock.unlock_irqrestore(fl);
    poll_notify();
}

uint32_t f_poll(File* f){
    USock* s = sock_of(f);
    uint64_t fl = ulock.lock_irqsave();
    uint32_t r = 0;
    if(s->state == St::Listen){
        r = s->nback ? POLLIN : 0;
    } else if(s->state == St::Conn){
        if(s->count || s->peer_gone || s->shut_rd){
            r |= POLLIN;
        }
        if(s->peer_gone){
            r |= POLLHUP;
        } else if(s->peer && space_in(s->peer) && !s->shut_wr){
            r |= POLLOUT;
        }
    }
    ulock.unlock_irqrestore(fl);
    return r;
}

const FileOps unix_ops = {f_read, f_write, f_close, nullptr, nullptr, f_poll};

File* new_file(USock* s, bool nonblock){
    File* f = file_alloc(&unix_ops, s);
    if(f){
        f->flags = O_RDWR | (nonblock ? O_NONBLOCK : 0);
    }
    return f;
}

bool nonblocking(File* f, int flags){
    return (flags & MSG_DONTWAIT) || (f->flags & O_NONBLOCK);
}

int check_type(int type, int proto){
    int base = type & ~SOCK_NONBLOCK;
    if(base != SOCK_STREAM){
        return -EPROTONOSUPPORT;
    }
    return proto ? -EPROTONOSUPPORT : 0;
}

}

bool unix_is(const File* f){
    return f && f->ops == &unix_ops;
}

uint64_t unix_count(){
    uint64_t fl = ulock.lock_irqsave();
    uint64_t n = live;
    ulock.unlock_irqrestore(fl);
    return n;
}

int unix_create(int type, int proto, File*& out){
    int err = check_type(type, proto);
    if(err){
        return err;
    }
    USock* s = salloc();
    if(!s){
        return -ENOMEM;
    }
    out = new_file(s, type & SOCK_NONBLOCK);
    if(!out){
        uint64_t fl = ulock.lock_irqsave();
        sput(s);
        ulock.unlock_irqrestore(fl);
        return -ENOMEM;
    }
    return 0;
}

int unix_pair(int type, int proto, File*& a, File*& b){
    int err = unix_create(type, proto, a);
    if(err){
        return err;
    }
    err = unix_create(type, proto, b);
    if(err){
        file_put(a);
        return err;
    }
    uint64_t fl = ulock.lock_irqsave();
    pair_up(sock_of(a), sock_of(b));
    ulock.unlock_irqrestore(fl);
    return 0;
}

int unix_bind(File* f, const char* path, uint32_t len){
    if(!len || len > UNIX_PATH_MAX){
        return -EINVAL;
    }
    USock* s = sock_of(f);
    SpinlockGuard g(ulock);
    if(s->bound || s->state != St::New){
        return -EINVAL;
    }
    if(find_bound(path, len)){
        return -EADDRINUSE;
    }
    memcpy(s->name, path, len);
    s->name_len = len;
    s->bound = true;
    s->next_bound = bound_list;
    bound_list = s;
    return 0;
}

int unix_listen(File* f, int backlog){
    USock* s = sock_of(f);
    SpinlockGuard g(ulock);
    if(!s->bound || (s->state != St::New && s->state != St::Listen)){
        return -EINVAL;
    }
    s->state = St::Listen;
    s->maxback = backlog <= 0 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog;
    return 0;
}

int unix_connect(File* f, const char* path, uint32_t len){
    if(!len || len > UNIX_PATH_MAX){
        return -EINVAL;
    }
    USock* c = sock_of(f);
    USock* srv = salloc();                      // capatul din server, intors de accept
    if(!srv){
        return -ENOMEM;
    }
    bool nb = nonblocking(f, 0);
    uint64_t fl = ulock.lock_irqsave();
    int err = 0;
    for(;;){
        if(c->state == St::Conn){
            err = -EISCONN;
            break;
        }
        if(c->state == St::Listen){
            err = -EINVAL;
            break;
        }
        USock* l = find_bound(path, len);
        if(!l || l->state != St::Listen){
            err = -ECONNREFUSED;
            break;
        }
        if(l->nback < l->maxback){
            memcpy(srv->name, l->name, l->name_len);
            srv->name_len = l->name_len;
            pair_up(c, srv);
            l->backlog[l->nback++] = srv;       // referinta lui srv trece in backlog
            srv = nullptr;
            wake(l);
            break;
        }
        if(nb){
            err = -EAGAIN;
            break;
        }
        sget(l);
        bool ok = l->wq.wait_intr(ulock);
        sput(l);
        if(!ok){
            err = -EINTR;
            break;
        }
    }
    if(srv){
        sput(srv);
    }
    ulock.unlock_irqrestore(fl);
    if(!err){
        poll_notify();
    }
    return err;
}

int unix_accept(File* f, File*& out){
    USock* l = sock_of(f);
    bool nb = nonblocking(f, 0);
    uint64_t fl = ulock.lock_irqsave();
    if(l->state != St::Listen){
        ulock.unlock_irqrestore(fl);
        return -EINVAL;
    }
    while(!l->nback){
        if(nb){
            ulock.unlock_irqrestore(fl);
            return -EAGAIN;
        }
        if(!l->wq.wait_intr(ulock) && !l->nback){
            ulock.unlock_irqrestore(fl);
            return -EINTR;
        }
        if(l->state != St::Listen){
            ulock.unlock_irqrestore(fl);
            return -EINVAL;
        }
    }
    USock* s = l->backlog[0];
    for(int i = 1; i < l->nback; i++){
        l->backlog[i - 1] = l->backlog[i];
    }
    l->nback--;
    wake(l);                                    // un connect blocat pe backlog plin poate continua
    ulock.unlock_irqrestore(fl);
    out = new_file(s, false);                   // referinta din backlog trece la File
    if(!out){
        fl = ulock.lock_irqsave();
        s->file_closed = true;
        disconnect(s);
        sput(s);
        ulock.unlock_irqrestore(fl);
        return -ENOMEM;
    }
    poll_notify();
    return 0;
}

int64_t unix_send(File* f, const void* buf, uint64_t len, int flags){
    USock* s = sock_of(f);
    bool nb = nonblocking(f, flags);
    const uint8_t* src = (const uint8_t*)buf;
    uint64_t done = 0;
    int64_t err = 0;
    uint64_t fl = ulock.lock_irqsave();
    while(done < len){
        if(s->state != St::Conn){
            err = -ENOTCONN;
            break;
        }
        USock* p = s->peer;
        if(s->shut_wr || !p || p->file_closed || p->shut_rd){
            err = -EPIPE;
            break;
        }
        uint32_t room = space_in(p);
        if(room){
            uint32_t n = len - done < room ? (uint32_t)(len - done) : room;
            ring_put(p, src + done, n);
            done += n;
            wake(p);
            continue;
        }
        if(nb){
            err = -EAGAIN;
            break;
        }
        // Asteapta loc: cititorul (p) trezeste coada lui s dupa ce consuma
        if(!s->wq.wait_intr(ulock)){
            err = -EINTR;
            break;
        }
    }
    ulock.unlock_irqrestore(fl);
    if(done){
        poll_notify();
        return (int64_t)done;
    }
    return err;
}

int64_t unix_recv(File* f, void* buf, uint64_t len, int flags){
    USock* s = sock_of(f);
    bool nb = nonblocking(f, flags);
    uint64_t fl = ulock.lock_irqsave();
    if(s->state != St::Conn){
        ulock.unlock_irqrestore(fl);
        return -ENOTCONN;
    }
    while(!s->count){
        if(s->peer_gone || s->shut_rd || (s->peer && s->peer->shut_wr)){
            ulock.unlock_irqrestore(fl);
            return 0;
        }
        if(nb){
            ulock.unlock_irqrestore(fl);
            return -EAGAIN;
        }
        if(!s->wq.wait_intr(ulock) && !s->count){
            ulock.unlock_irqrestore(fl);
            return -EINTR;
        }
    }
    uint32_t n = len < s->count ? (uint32_t)len : s->count;
    bool peek = flags & MSG_PEEK;
    ring_get(s, (uint8_t*)buf, n, peek);
    if(!peek){
        wake(s->peer);                          // scriitorul are iar loc
    }
    ulock.unlock_irqrestore(fl);
    if(!peek){
        poll_notify();
    }
    return n;
}

int unix_shutdown(File* f, int how){
    USock* s = sock_of(f);
    uint64_t fl = ulock.lock_irqsave();
    if(s->state != St::Conn){
        ulock.unlock_irqrestore(fl);
        return -ENOTCONN;
    }
    if(how == SHUT_RD || how == SHUT_RDWR){
        s->shut_rd = true;
    }
    if(how == SHUT_WR || how == SHUT_RDWR){
        s->shut_wr = true;
    }
    wake(s);
    wake(s->peer);
    ulock.unlock_irqrestore(fl);
    poll_notify();
    return 0;
}

int unix_name(File* f, bool peer, char* path, uint32_t& len){
    USock* s = sock_of(f);
    SpinlockGuard g(ulock);
    const USock* x = s;
    if(peer){
        if(s->state != St::Conn || !s->peer){
            return -ENOTCONN;
        }
        x = s->peer;
    }
    len = x->name_len;
    memcpy(path, x->name, len);
    return 0;
}

int unix_getopt(File* f, int level, int opt, void* val, uint32_t* len){
    (void)f;
    if(level != SOL_SOCKET){
        return -ENOPROTOOPT;
    }
    int v;
    switch(opt){
    case SO_TYPE:
        v = SOCK_STREAM;
        break;
    case SO_ERROR:
        v = 0;
        break;
    case SO_RCVBUF:
    case SO_SNDBUF:
        v = (int)UNIX_BUF;
        break;
    default:
        return -ENOPROTOOPT;
    }
    uint32_t n = *len < sizeof(v) ? *len : sizeof(v);
    memcpy(val, &v, n);
    *len = n;
    return 0;
}

int unix_setopt(File*, int level, int opt, const void*, uint32_t){
    if(level != SOL_SOCKET){
        return -ENOPROTOOPT;
    }
    switch(opt){
    case SO_REUSEADDR:
    case SO_RCVBUF:
    case SO_SNDBUF:
    case SO_KEEPALIVE:
        return 0;                               // acceptate, fara efect
    default:
        return -ENOPROTOOPT;
    }
}
