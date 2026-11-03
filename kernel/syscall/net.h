#pragma once
#include "types.h"

// Syscall-urile de retea (net.cpp): socket-uri BSD, poll, select. Legate in table.cpp
int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto, uint64_t, uint64_t, uint64_t);
int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t);
int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t, uint64_t, uint64_t);
int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t alen);
int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t lenp);
int64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t);
int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t, uint64_t, uint64_t);
int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t, uint64_t, uint64_t);
int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t len, uint64_t);
int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t lenp, uint64_t);
// ioctl pe un socket (T9.5): SIOCGIF* / SIOCSIF* / SIOCADDRT / SIOCDELRT. Chemat din sys_ioctl cand fd-ul e socket
struct File;
int64_t net_ioctl(File* f, uint64_t cmd, uint64_t arg);
int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms, uint64_t, uint64_t, uint64_t);
int64_t sys_select(uint64_t nfds, uint64_t rd, uint64_t wr, uint64_t ex, uint64_t timeout, uint64_t);
