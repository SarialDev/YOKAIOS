#ifndef _POLL_H
#define _POLL_H
/* poll peste orice fd: socket-uri, pipe-uri, consola, fisiere (fisierele obisnuite sunt mereu gata) */
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
/* timeout in ms; -1 = fara limita; fd negativ e ignorat */
int poll(struct pollfd *fds, nfds_t n, int timeout_ms);
#endif
