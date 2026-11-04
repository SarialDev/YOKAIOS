// Syscall-uri de retea (T9.3): socket-uri BSD peste net/socket.cpp, plus poll / select pentru orice fd (socket, pipe,
// consola, fisiere VFS). Adresele sunt struct sockaddr_in de pe Linux (uapi/net.h).
#include "syscall/net.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "uapi/net.h"
#include "uapi/fs.h"
#include "net/socket.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "drivers/tty.h"
#include "fs/pseudofs.h"
#include "proc/caps.h"
#include "hal/net.h"
#include "net/lwip_port.h"
#include "net/netcfg.h"
#include "net/unix.h"
#include "ipc/shm.h"
#include "sync/pollwait.h"

namespace {

const uint32_t SOCKADDR_LEN = sizeof(k_sockaddr_in);
const int POLL_MAX = PROC_MAX_FD;

// Socket-ul de la fd; err = -EBADF / -ENOTSOCK
File* sock_of(uint64_t fd, int64_t& err){
    Process* p = process_current();
    File* f = p ? fd_get(p->fds, (int64_t)fd) : nullptr;
    if(!f){
        err = -EBADF;
        return nullptr;
    }
    if(!sock_is(f)){
        err = -ENOTSOCK;
        return nullptr;
    }
    return f;
}

int addr_from_user(uint64_t addr, uint64_t len, KSockAddr& out){
    if(len < SOCKADDR_LEN){
        return -EINVAL;
    }
    k_sockaddr_in sa;
    if(!copy_from_user(&sa, addr, sizeof(sa))){
        return -EFAULT;
    }
    if(sa.sin_family != AF_INET){
        return -EAFNOSUPPORT;
    }
    out.ip = sa.sin_addr;
    out.port = sa.sin_port;
    return 0;
}

// Scrie adresa si lungimea ei (taiata la *lenp primit); addr == 0 = nu vrea adresa
int addr_to_user(uint64_t addr, uint64_t lenp, const KSockAddr& a){
    if(!addr || !lenp){
        return 0;
    }
    uint32_t have;
    if(!copy_from_user(&have, lenp, sizeof(have))){
        return -EFAULT;
    }
    k_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = a.port;
    sa.sin_addr = a.ip;
    uint32_t n = have < SOCKADDR_LEN ? have : SOCKADDR_LEN;
    uint32_t full = SOCKADDR_LEN;
    if(!copy_to_user(addr, &sa, n) || !copy_to_user(lenp, &full, sizeof(full))){
        return -EFAULT;
    }
    return 0;
}

// --- AF_UNIX (T10.2): struct sockaddr_un = {uint16_t sun_family; char sun_path[108]} ---

const uint32_t SUN_HDR = 2;

File* unix_file(uint64_t fd){
    Process* p = process_current();
    File* f = p ? fd_get(p->fds, (int64_t)fd) : nullptr;
    return unix_is(f) ? f : nullptr;
}

// Calea din sockaddr_un: pana la primul '\0' (nume in sistemul de fisiere) sau toti octetii (abstract, '\0' primul)
int sun_from_user(uint64_t addr, uint64_t len, char* path, uint32_t& plen){
    if(len <= SUN_HDR || len > SUN_HDR + UNIX_PATH_MAX){
        return -EINVAL;
    }
    uint8_t raw[SUN_HDR + UNIX_PATH_MAX];
    if(!copy_from_user(raw, addr, len)){
        return -EFAULT;
    }
    uint16_t fam = (uint16_t)(raw[0] | raw[1] << 8);
    if(fam != AF_UNIX){
        return -EAFNOSUPPORT;
    }
    uint32_t n = (uint32_t)len - SUN_HDR;
    const char* src = (const char*)raw + SUN_HDR;
    if(src[0]){
        uint32_t k = 0;
        while(k < n && src[k]){
            k++;
        }
        n = k;
    }
    memcpy(path, src, n);
    plen = n;
    return 0;
}

int sun_to_user(uint64_t addr, uint64_t lenp, const char* path, uint32_t plen){
    if(!addr || !lenp){
        return 0;
    }
    uint32_t have;
    if(!copy_from_user(&have, lenp, sizeof(have))){
        return -EFAULT;
    }
    uint8_t raw[SUN_HDR + UNIX_PATH_MAX + 1];
    memset(raw, 0, sizeof(raw));
    raw[0] = AF_UNIX;
    memcpy(raw + SUN_HDR, path, plen);
    // Ca pe Linux: numele din sistemul de fisiere se intoarce cu terminator
    uint32_t full = SUN_HDR + plen + (plen && path[0] ? 1 : 0);
    if(full > SUN_HDR + UNIX_PATH_MAX){
        full = SUN_HDR + UNIX_PATH_MAX;
    }
    uint32_t n = have < full ? have : full;
    if(!copy_to_user(addr, raw, n) || !copy_to_user(lenp, &full, sizeof(full))){
        return -EFAULT;
    }
    return 0;
}

int64_t unix_sendto(File* f, uint64_t buf, uint64_t len, uint64_t flags){
    if(!user_access_ok(buf, len, false)){
        return -EFAULT;
    }
    uint8_t* tmp = (uint8_t*)kmalloc(SOCK_IO_MAX);
    if(!tmp){
        return -ENOMEM;
    }
    int64_t total = 0, result = 0;
    while((uint64_t)total < len){
        uint64_t n = len - (uint64_t)total < SOCK_IO_MAX ? len - (uint64_t)total : SOCK_IO_MAX;
        if(!copy_from_user(tmp, buf + (uint64_t)total, n)){
            result = total ? total : -EFAULT;
            break;
        }
        int64_t r = unix_send(f, tmp, n, (int)flags);
        if(r < 0){
            result = total ? total : r;
            break;
        }
        total += r;
        result = total;
        if((uint64_t)r < n){
            break;
        }
    }
    kfree(tmp);
    if(result == -EPIPE && !(flags & MSG_NOSIGNAL)){
        signal_send_self(SIGPIPE);
    }
    return result;
}

int64_t unix_recvfrom(File* f, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t lenp){
    if(!user_access_ok(buf, len, true)){
        return -EFAULT;
    }
    uint64_t want = len < SOCK_IO_MAX ? len : SOCK_IO_MAX;
    uint8_t* tmp = (uint8_t*)kmalloc(want ? want : 1);
    if(!tmp){
        return -ENOMEM;
    }
    int64_t r = want ? unix_recv(f, tmp, want, (int)flags) : 0;
    if(r > 0 && !copy_to_user(buf, tmp, (uint64_t)r)){
        r = -EFAULT;
    }
    kfree(tmp);
    if(r >= 0){
        sun_to_user(addr, lenp, "", 0);         // partenerul unui stream: fara nume
    }
    return r;
}

}

int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t proto, uint64_t sv, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    if(domain != AF_UNIX){
        return domain == AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
    }
    if(!user_access_ok(sv, 2 * sizeof(int), true)){
        return -EFAULT;
    }
    File* a;
    File* b;
    int err = unix_pair((int)type, (int)proto, a, b);
    if(err){
        return err;
    }
    int fds[2];
    fds[0] = fd_install(p->fds, a);
    if(fds[0] < 0){
        file_put(a);
        file_put(b);
        return fds[0];
    }
    fds[1] = fd_install(p->fds, b);
    if(fds[1] < 0){
        fd_close(p->fds, fds[0]);
        file_put(b);
        return fds[1];
    }
    if(!copy_to_user(sv, fds, sizeof(fds))){
        fd_close(p->fds, fds[0]);
        fd_close(p->fds, fds[1]);
        return -EFAULT;
    }
    return 0;
}

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    if(domain == AF_UNIX){
        File* uf;
        int uerr = unix_create((int)type, (int)proto, uf);
        if(uerr){
            return uerr;
        }
        int ufd = fd_install(p->fds, uf);
        if(ufd < 0){
            file_put(uf);
        }
        return ufd;
    }
    if(domain != AF_INET){
        return -EAFNOSUPPORT;
    }
    if(((int)type & ~SOCK_NONBLOCK) == SOCK_RAW && !capable(CAP_NET)){
        return -EPERM;                      // socket brut (ping): ca pe Linux, doar cu privilegii
    }
    File* f;
    int err = sock_create((int)type, (int)proto, f);
    if(err){
        return err;
    }
    int fd = fd_install(p->fds, f);
    if(fd < 0){
        file_put(f);
    }
    return fd;
}

int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        char path[UNIX_PATH_MAX];
        uint32_t plen;
        int e = sun_from_user(addr, len, path, plen);
        return e ? e : unix_bind(uf, path, plen);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    KSockAddr a;
    if(!f){
        return err;
    }
    err = addr_from_user(addr, len, a);
    return err ? err : sock_bind(f, a);
}

int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        return unix_listen(uf, (int)backlog);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    return f ? sock_listen(f, (int)backlog) : err;
}

int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        char path[UNIX_PATH_MAX];
        uint32_t plen;
        int e = sun_from_user(addr, len, path, plen);
        return e ? e : unix_connect(uf, path, plen);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    KSockAddr a;
    if(!f){
        return err;
    }
    err = addr_from_user(addr, len, a);
    return err ? err : sock_connect(f, a);
}

int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        Process* up = process_current();
        File* nf;
        int e = unix_accept(uf, nf);
        if(e){
            return e;
        }
        int nfd = fd_install(up->fds, nf);
        if(nfd < 0){
            file_put(nf);
            return nfd;
        }
        sun_to_user(addr, lenp, "", 0);
        return nfd;
    }
    Process* p = process_current();
    int64_t err;
    File* f = sock_of(fd, err);
    if(!f){
        return err;
    }
    File* nf;
    KSockAddr peer;
    err = sock_accept(f, nf, peer);
    if(err){
        return err;
    }
    int nfd = fd_install(p->fds, nf);
    if(nfd < 0){
        file_put(nf);
        return nfd;
    }
    addr_to_user(addr, lenp, peer);
    return nfd;
}

int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t alen){
    if(File* uf = unix_file(fd)){
        (void)addr;
        (void)alen;
        return unix_sendto(uf, buf, len, flags);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    if(!f){
        return err;
    }
    if(!user_access_ok(buf, len, false)){
        return -EFAULT;
    }
    KSockAddr to;
    if(addr){
        err = addr_from_user(addr, alen, to);
        if(err){
            return err;
        }
    }
    bool stream = sock_is_stream(f);
    if(!stream && len > SOCK_IO_MAX * 2){
        return -EMSGSIZE;
    }
    uint64_t chunk = stream ? SOCK_IO_MAX : SOCK_IO_MAX * 2;
    uint64_t want = len < chunk ? len : chunk;
    uint8_t* tmp = (uint8_t*)kmalloc(want ? want : 1);
    if(!tmp){
        return -ENOMEM;
    }
    int64_t total = 0;
    int64_t result = 0;
    do {
        uint64_t n = len - (uint64_t)total < chunk ? len - (uint64_t)total : chunk;
        if(!copy_from_user(tmp, buf + (uint64_t)total, n)){
            result = total ? total : -EFAULT;
            break;
        }
        int64_t r = sock_sendto(f, tmp, n, (int)flags, addr ? &to : nullptr);
        if(r < 0){
            result = total ? total : r;
            break;
        }
        total += r;
        result = total;
        if((uint64_t)r < n){
            break;
        }
    } while((uint64_t)total < len);
    kfree(tmp);
    if(result == -EPIPE && !(flags & MSG_NOSIGNAL)){
        signal_send_self(SIGPIPE);
    }
    return result;
}

int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t lenp){
    if(File* uf = unix_file(fd)){
        return unix_recvfrom(uf, buf, len, flags, addr, lenp);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    if(!f){
        return err;
    }
    if(!user_access_ok(buf, len, true)){
        return -EFAULT;
    }
    uint64_t chunk = sock_is_stream(f) ? SOCK_IO_MAX : SOCK_IO_MAX * 2;
    uint64_t want = len < chunk ? len : chunk;
    uint8_t* tmp = (uint8_t*)kmalloc(want ? want : 1);
    if(!tmp){
        return -ENOMEM;
    }
    KSockAddr from;
    bool has_from;
    int64_t r = want ? sock_recvfrom(f, tmp, want, (int)flags, &from, &has_from) : 0;
    if(r > 0 && !copy_to_user(buf, tmp, (uint64_t)r)){
        r = -EFAULT;
    }
    kfree(tmp);
    if(r >= 0 && has_from){
        addr_to_user(addr, lenp, from);
    }
    return r;
}

int64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        return how > SHUT_RDWR ? -EINVAL : unix_shutdown(uf, (int)how);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    if(!f){
        return err;
    }
    return how > SHUT_RDWR ? -EINVAL : sock_shutdown(f, (int)how);
}

int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        char path[UNIX_PATH_MAX];
        uint32_t plen;
        int e = unix_name(uf, false, path, plen);
        return e ? e : sun_to_user(addr, lenp, path, plen);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    KSockAddr a;
    if(!f){
        return err;
    }
    err = sock_name(f, false, a);
    return err ? err : addr_to_user(addr, lenp, a);
}

int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t, uint64_t, uint64_t){
    if(File* uf = unix_file(fd)){
        char path[UNIX_PATH_MAX];
        uint32_t plen;
        int e = unix_name(uf, true, path, plen);
        return e ? e : sun_to_user(addr, lenp, path, plen);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    KSockAddr a;
    if(!f){
        return err;
    }
    err = sock_name(f, true, a);
    return err ? err : addr_to_user(addr, lenp, a);
}

int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t len, uint64_t){
    if(File* uf = unix_file(fd)){
        uint8_t ubuf[16];
        if(len > sizeof(ubuf)){
            return -EINVAL;
        }
        if(!copy_from_user(ubuf, val, len)){
            return -EFAULT;
        }
        return unix_setopt(uf, (int)level, (int)opt, ubuf, (uint32_t)len);
    }
    int64_t err;
    File* f = sock_of(fd, err);
    if(!f){
        return err;
    }
    uint8_t tmp[16];
    if(len > sizeof(tmp)){
        return -EINVAL;
    }
    if(!copy_from_user(tmp, val, len)){
        return -EFAULT;
    }
    return sock_setopt(f, (int)level, (int)opt, tmp, (uint32_t)len);
}

int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t lenp, uint64_t){
    if(File* uf = unix_file(fd)){
        uint32_t ulen;
        uint8_t ubuf[16];
        if(!copy_from_user(&ulen, lenp, sizeof(ulen))){
            return -EFAULT;
        }
        if(ulen > sizeof(ubuf)){
            ulen = sizeof(ubuf);
        }
        int e = unix_getopt(uf, (int)level, (int)opt, ubuf, &ulen);
        if(e){
            return e;
        }
        return copy_to_user(val, ubuf, ulen) && copy_to_user(lenp, &ulen, sizeof(ulen)) ? 0 : -EFAULT;
    }
    int64_t err;
    File* f = sock_of(fd, err);
    if(!f){
        return err;
    }
    uint32_t len;
    uint8_t tmp[16];
    if(!copy_from_user(&len, lenp, sizeof(len))){
        return -EFAULT;
    }
    if(len > sizeof(tmp)){
        len = sizeof(tmp);
    }
    err = sock_getopt(f, (int)level, (int)opt, tmp, &len);
    if(err){
        return err;
    }
    return copy_to_user(val, tmp, len) && copy_to_user(lenp, &len, sizeof(len)) ? 0 : -EFAULT;
}

// --- ioctl pe socket-uri: interfete si rute ---

namespace {

// Interfata cu numele din ifreq: indicele HAL, sau -1
int find_if(const k_ifreq& r){
    char name[IFNAMSIZ + 1];
    memcpy(name, r.ifr_name, IFNAMSIZ);
    name[IFNAMSIZ] = 0;
    for(int i = 0; i < hal_count(HalKind::Net); i++){
        const char* n = hal_name(HalKind::Net, i);
        if(n){
            int k = 0;
            while(n[k] && n[k] == name[k]){
                k++;
            }
            if(n[k] == name[k]){
                return i;
            }
        }
    }
    return -1;
}

void put_addr(k_ifreq& r, uint32_t ip){
    memset(&r.u, 0, sizeof(r.u));
    r.u.addr.sa_family = AF_INET;
    memcpy(&r.u.addr.sa_data[2], &ip, 4);       // sockaddr_in: port (2 octeti), apoi adresa
}

bool get_addr(const k_ifreq& r, uint32_t& ip){
    if(r.u.addr.sa_family != AF_INET){
        return false;
    }
    memcpy(&ip, &r.u.addr.sa_data[2], 4);
    return true;
}

}

int64_t net_ioctl(File*, uint64_t cmd, uint64_t arg){
    if(cmd == SIOCADDRT || cmd == SIOCDELRT){
        if(!capable(CAP_NET)){
            return -EPERM;
        }
        k_rtentry rt;
        if(!copy_from_user(&rt, arg, sizeof(rt))){
            return -EFAULT;
        }
        // Doar ruta implicita (destinatie 0.0.0.0, masca 0.0.0.0) prin gateway: restul rutelor vin din adresa interfetei
        uint32_t dst = 0, mask = 0, gw = 0;
        memcpy(&dst, &rt.rt_dst.sa_data[2], 4);
        memcpy(&mask, &rt.rt_genmask.sa_data[2], 4);
        memcpy(&gw, &rt.rt_gateway.sa_data[2], 4);
        if(dst != 0 || mask != 0){
            return -EOPNOTSUPP;
        }
        if(cmd == SIOCADDRT){
            if(!(rt.rt_flags & RTF_GATEWAY) || rt.rt_gateway.sa_family != AF_INET || gw == 0){
                return -EINVAL;
            }
            return netcfg_set(false, 0, false, 0, true, gw);
        }
        return netcfg_set(false, 0, false, 0, true, 0);
    }
    k_ifreq r;
    if(!copy_from_user(&r, arg, sizeof(r))){
        return -EFAULT;
    }
    int idx = find_if(r);
    if(idx < 0){
        return -ENODEV;
    }
    NetDevice* dev = hal_net(idx);
    bool primary = dev && dev == net_device() && net_ready();
    NetCfgInfo n;
    netcfg_info(n);
    bool have = primary && n.bound;
    switch(cmd){
    case SIOCGIFFLAGS: {
        r.u.flags = (short)(IFF_BROADCAST | IFF_MULTICAST | (have ? IFF_UP : 0) | (dev && dev->link_up() ? IFF_RUNNING : 0));
        break;
    }
    case SIOCGIFADDR:
        if(!have){
            return -EADDRNOTAVAIL;
        }
        put_addr(r, n.addr);
        break;
    case SIOCGIFNETMASK:
        if(!have){
            return -EADDRNOTAVAIL;
        }
        put_addr(r, n.mask);
        break;
    case SIOCGIFBRDADDR:
        if(!have){
            return -EADDRNOTAVAIL;
        }
        put_addr(r, n.addr | ~n.mask);
        break;
    case SIOCGIFMTU:
        r.u.mtu = dev ? (int)dev->mtu() : 0;
        break;
    case SIOCGIFHWADDR: {
        uint8_t mac[6] = {};
        if(dev){
            dev->mac(mac);
        }
        memset(&r.u, 0, sizeof(r.u));
        r.u.addr.sa_family = ARPHRD_ETHER;
        memcpy(r.u.addr.sa_data, mac, 6);
        break;
    }
    case SIOCSIFADDR:
    case SIOCSIFNETMASK: {
        if(!capable(CAP_NET)){
            return -EPERM;
        }
        if(!primary){
            return -ENODEV;
        }
        uint32_t v;
        if(!get_addr(r, v)){
            return -EAFNOSUPPORT;
        }
        return cmd == SIOCSIFADDR ? netcfg_set(true, v, false, 0, false, 0) : netcfg_set(false, 0, true, v, false, 0);
    }
    case SIOCSIFFLAGS:
        // Interfata nu se poate opri; IFF_UP e acceptat (deja sus), restul ignorat
        return capable(CAP_NET) ? 0 : -EPERM;
    default:
        return -ENOTTY;
    }
    return copy_to_user(arg, &r, sizeof(r)) ? 0 : -EFAULT;
}

// --- poll / select ---

namespace {

struct Ent {
    File* f;                    // nullptr = ignorat (fd negativ) sau POLLNVAL
    uint32_t ev;
    uint32_t rev;
    bool sock;
};

// Starea unui fisier care nu e socket
uint32_t file_mask(File* f){
    if(f == console_file() || devfs_is_console(f->inode)){
        return POLLOUT | (tty_readable() ? POLLIN : 0);
    }
    if(f->ops->poll){
        return f->ops->poll(f);
    }
    return POLLIN | POLLOUT;
}

// Asteapta pana cand cel putin o intrare are evenimente, timeout_ms (< 0 = fara limita) sau un semnal. Intoarce cate intrari
// au evenimente, 0 la expirare sau -EINTR. Socket-urile lwIP asteapta in lwIP (trezire imediata; fd-urile de alt fel se
// sondeaza atunci din ms in ms), altfel se doarme in poll_wait pana la un poll_notify
int64_t poll_engine(Ent* e, int n, int64_t timeout_ms){
    uint64_t end = timeout_ms > 0 ? monotonic_ns() + (uint64_t)timeout_ms * 1000000UL : 0;
    for(;;){
        uint64_t seq = poll_seq();              // inainte de verificare: o schimbare de acum incolo trezeste poll_wait
        File* sf[POLL_MAX];
        uint32_t sev[POLL_MAX], srev[POLL_MAX];
        int sidx[POLL_MAX];
        int ns = 0;
        bool others = false;
        int ready = 0;
        for(int i = 0; i < n; i++){
            e[i].rev = 0;
            if(!e[i].f){
                continue;
            }
            if(e[i].sock){
                sf[ns] = e[i].f;
                sev[ns] = e[i].ev;
                sidx[ns++] = i;
            } else {
                others = true;
                e[i].rev = file_mask(e[i].f) & (e[i].ev | POLLERR | POLLHUP);
                ready += e[i].rev != 0;
            }
        }
        int64_t tmo = 0;
        if(!ready && timeout_ms != 0){
            int64_t slice = others ? 1 : 20;
            if(end){
                uint64_t now = monotonic_ns();
                int64_t rem = now >= end ? 0 : (int64_t)((end - now + 999999UL) / 1000000UL);
                tmo = rem < slice ? rem : slice;
            } else {
                tmo = slice;
            }
        }
        if(ns){
            sock_poll_wait(sf, sev, srev, ns, (int)tmo);
            for(int j = 0; j < ns; j++){
                Ent& x = e[sidx[j]];
                x.rev = srev[j] & (x.ev | POLLERR | POLLHUP | POLLNVAL);
            }
        } else if(!ready && timeout_ms != 0){
            // Fara socket-uri lwIP: pipe, terminal, AF_UNIX, input anunta schimbarile prin poll_notify (fara sondare)
            poll_wait(seq, end);
        }
        int count = 0;
        for(int i = 0; i < n; i++){
            count += e[i].rev != 0;
        }
        if(count){
            return count;
        }
        if(timeout_ms == 0 || (end && monotonic_ns() >= end)){
            return 0;
        }
        if(signal_pending()){
            return -EINTR;
        }
    }
}

File* fd_file(int64_t fd){
    Process* p = process_current();
    return p ? fd_get(p->fds, fd) : nullptr;
}

}

int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms, uint64_t, uint64_t, uint64_t){
    if(nfds > (uint64_t)POLL_MAX * 2){
        return -EINVAL;
    }
    int n = (int)nfds;
    k_pollfd pf[POLL_MAX * 2];
    Ent e[POLL_MAX * 2];
    if(n && !copy_from_user(pf, fds, n * sizeof(k_pollfd))){
        return -EFAULT;
    }
    // Cel mult POLL_MAX socket-uri sunt asteptate deodata (cate fd-uri are un proces)
    int socks = 0;
    for(int i = 0; i < n; i++){
        e[i].f = pf[i].fd >= 0 ? fd_file(pf[i].fd) : nullptr;
        e[i].ev = (uint32_t)(uint16_t)pf[i].events;
        e[i].rev = 0;
        e[i].sock = e[i].f && sock_is(e[i].f);
        socks += e[i].sock;
    }
    if(socks > POLL_MAX){
        return -EINVAL;
    }
    int64_t r = poll_engine(e, n, (int32_t)timeout_ms);       // int timeout: -1 vine cu bitii de sus zero
    if(r < 0){
        return r;
    }
    int64_t count = 0;
    for(int i = 0; i < n; i++){
        uint32_t rev = e[i].rev;
        if(pf[i].fd >= 0 && !e[i].f){
            rev = POLLNVAL;
        }
        pf[i].revents = (short)rev;
        count += rev != 0;
    }
    if(n && !copy_to_user(fds, pf, n * sizeof(k_pollfd))){
        return -EFAULT;
    }
    return count;
}

int64_t sys_select(uint64_t nfds, uint64_t rd, uint64_t wr, uint64_t ex, uint64_t timeout, uint64_t){
    if(nfds > FD_SETSIZE){
        return -EINVAL;
    }
    const uint64_t words = (nfds + 63) / 64;
    uint64_t set[3][FD_SETSIZE / 64];
    const uint64_t user[3] = {rd, wr, ex};
    memset(set, 0, sizeof(set));
    for(int k = 0; k < 3; k++){
        if(user[k] && words && !copy_from_user(set[k], user[k], words * 8)){
            return -EFAULT;
        }
    }
    int64_t timeout_ms = -1;
    if(timeout){
        k_timeval tv;
        if(!copy_from_user(&tv, timeout, sizeof(tv))){
            return -EFAULT;
        }
        if(tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000){
            return -EINVAL;
        }
        timeout_ms = tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
    }
    Ent e[POLL_MAX];
    int fdnum[POLL_MAX];
    int n = 0;
    int socks = 0;
    for(uint64_t fd = 0; fd < nfds; fd++){
        uint32_t ev = 0;
        if(set[0][fd / 64] >> (fd % 64) & 1){
            ev |= POLLIN;
        }
        if(set[1][fd / 64] >> (fd % 64) & 1){
            ev |= POLLOUT;
        }
        if(set[2][fd / 64] >> (fd % 64) & 1){
            ev |= POLLPRI;
        }
        if(!ev){
            continue;
        }
        File* f = fd < (uint64_t)POLL_MAX ? fd_file((int64_t)fd) : nullptr;
        if(!f || n == POLL_MAX){
            return -EBADF;
        }
        e[n].f = f;
        e[n].ev = ev;
        e[n].rev = 0;
        e[n].sock = sock_is(f);
        socks += e[n].sock;
        fdnum[n++] = (int)fd;
    }
    int64_t r = poll_engine(e, n, timeout_ms);
    if(r < 0){
        return r;
    }
    memset(set, 0, sizeof(set));
    int64_t count = 0;
    for(int i = 0; i < n; i++){
        uint64_t bit = 1UL << (fdnum[i] % 64);
        uint32_t rev = e[i].rev;
        if(rev & (POLLIN | POLLHUP | POLLERR)){
            if(e[i].ev & POLLIN){
                set[0][fdnum[i] / 64] |= bit;
                count++;
            }
        }
        if(rev & (POLLOUT | POLLERR)){
            if(e[i].ev & POLLOUT){
                set[1][fdnum[i] / 64] |= bit;
                count++;
            }
        }
    }
    for(int k = 0; k < 3; k++){
        if(user[k] && words && !copy_to_user(user[k], set[k], words * 8)){
            return -EFAULT;
        }
    }
    return count;
}
