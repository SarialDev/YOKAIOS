#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
/* Doar IPv4, forma a.b.c.d */
in_addr_t inet_addr(const char *s);                 /* INADDR_NONE la eroare */
int inet_aton(const char *s, struct in_addr *out);  /* 1 = ok */
char *inet_ntoa(struct in_addr a);                  /* buffer static */
int inet_pton(int af, const char *src, void *dst);  /* 1 = ok, 0 = format gresit, -1 = af necunoscut (EAFNOSUPPORT) */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
#endif
