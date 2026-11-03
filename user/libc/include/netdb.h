#ifndef _NETDB_H
#define _NETDB_H
#include <sys/socket.h>
#include <netinet/in.h>
/* Fara DNS (lwIP e construit fara): rezolvarea merge pentru adrese numerice si "localhost"; serviciile dintr-un tabel mic
 * (echo, daytime, ftp, ssh, telnet, smtp, domain, http, https) sau numere */
struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]
struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;                 /* network order */
    char *s_proto;
};
struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
#define AI_PASSIVE 0x01
#define AI_CANONNAME 0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x400
#define EAI_BADFLAGS (-1)
#define EAI_NONAME (-2)
#define EAI_AGAIN (-3)
#define EAI_FAIL (-4)
#define EAI_FAMILY (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE (-8)
#define EAI_MEMORY (-10)
#define HOST_NOT_FOUND 1
#define NO_DATA 4
extern int h_errno;
struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *proto);
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int code);
#endif
