#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>
#define FD_SETSIZE 1024
typedef struct { unsigned long __bits[FD_SETSIZE / 64]; } fd_set;
#define FD_ZERO(s) do { for(int __i = 0; __i < FD_SETSIZE / 64; __i++) (s)->__bits[__i] = 0; } while(0)
#define FD_SET(fd, s) ((s)->__bits[(fd) / 64] |= 1UL << ((fd) % 64))
#define FD_CLR(fd, s) ((s)->__bits[(fd) / 64] &= ~(1UL << ((fd) % 64)))
#define FD_ISSET(fd, s) (((s)->__bits[(fd) / 64] >> ((fd) % 64)) & 1)
/* Timpul ramas nu se actualizeaza in timeout */
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);
#endif
