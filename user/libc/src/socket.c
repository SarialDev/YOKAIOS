/* Socket-uri BSD (T9.3): send/recv peste sendto/recvfrom, adrese IPv4, rezolvare numerica (fara DNS) */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int h_errno;

ssize_t send(int fd, const void *buf, size_t n, int flags){
    return sendto(fd, buf, n, flags, NULL, 0);
}

ssize_t recv(int fd, void *buf, size_t n, int flags){
    return recvfrom(fd, buf, n, flags, NULL, NULL);
}

/* a.b.c.d exact (zecimal, fara zerouri in fata inutile, fiecare 0..255); end = primul caracter dupa adresa */
static int parse_ipv4(const char *s, uint32_t *out, const char **end){
    uint32_t v = 0;
    for(int i = 0; i < 4; i++){
        if(*s < '0' || *s > '9'){
            return 0;
        }
        unsigned part = 0;
        int digits = 0;
        const char *start = s;
        while(*s >= '0' && *s <= '9'){
            part = part * 10 + (unsigned)(*s++ - '0');
            if(++digits > 3 || part > 255){
                return 0;
            }
        }
        if(digits > 1 && *start == '0'){
            return 0;
        }
        v |= part << (8 * i);                /* primul octet = cel mai mic: ordinea retelei pe un host little-endian */
        if(i < 3){
            if(*s != '.'){
                return 0;
            }
            s++;
        }
    }
    *out = v;
    *end = s;
    return 1;
}

int inet_aton(const char *s, struct in_addr *out){
    uint32_t v;
    const char *end;
    if(!parse_ipv4(s, &v, &end) || (*end && *end != ' ' && *end != '\t' && *end != '\n')){
        return 0;
    }
    out->s_addr = v;
    return 1;
}

in_addr_t inet_addr(const char *s){
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr a){
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

int inet_pton(int af, const char *src, void *dst){
    if(af != AF_INET){
        errno = EAFNOSUPPORT;
        return -1;
    }
    uint32_t v;
    const char *end;
    if(!parse_ipv4(src, &v, &end) || *end){
        return 0;
    }
    memcpy(dst, &v, 4);
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size){
    if(af != AF_INET){
        errno = EAFNOSUPPORT;
        return NULL;
    }
    const unsigned char *b = src;
    char tmp[INET_ADDRSTRLEN];
    int n = snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if((socklen_t)n >= size){
        errno = ENOSPC;
        return NULL;
    }
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

static int resolve(const char *name, uint32_t *ip){
    const char *end;
    if(parse_ipv4(name, ip, &end) && !*end){
        return 1;
    }
    if(strcmp(name, "localhost") == 0){
        *ip = INADDR_LOOPBACK;
        return 1;
    }
    return 0;
}

struct hostent *gethostbyname(const char *name){
    static struct hostent h;
    static char hname[64];
    static char *aliases[1];
    static char *addrs[2];
    static uint32_t ip;
    if(!name || !resolve(name, &ip)){
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    strncpy(hname, name, sizeof(hname) - 1);
    h.h_name = hname;
    h.h_aliases = aliases;
    h.h_addrtype = AF_INET;
    h.h_length = 4;
    addrs[0] = (char *)&ip;
    h.h_addr_list = addrs;
    return &h;
}

static const struct { const char *name; int port; } services[] = {
    {"echo", 7}, {"daytime", 13}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"domain", 53}, {"http", 80},
    {"https", 443},
};

static int service_port(const char *s){
    if(*s >= '0' && *s <= '9'){
        char *end;
        unsigned long v = strtoul(s, &end, 10);
        return !*end && v <= 65535 ? (int)v : -1;
    }
    for(unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++){
        if(strcmp(services[i].name, s) == 0){
            return services[i].port;
        }
    }
    return -1;
}

struct servent *getservbyname(const char *name, const char *proto){
    static struct servent se;
    static char sname[16];
    static char *aliases[1];
    int port = -1;
    for(unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++){
        if(strcmp(services[i].name, name) == 0){
            port = services[i].port;
        }
    }
    if(port < 0){
        return NULL;
    }
    strncpy(sname, name, sizeof(sname) - 1);
    se.s_name = sname;
    se.s_aliases = aliases;
    se.s_port = htons((uint16_t)port);
    se.s_proto = (char *)(proto ? proto : "tcp");
    return &se;
}

struct ai_node {
    struct addrinfo ai;
    struct sockaddr_in sa;
};

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res){
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    if(family != AF_UNSPEC && family != AF_INET){
        return EAI_FAMILY;
    }
    if(socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM){
        return EAI_SOCKTYPE;
    }
    if(!node && !service){
        return EAI_NONAME;
    }
    uint32_t ip = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    if(node && !resolve(node, &ip)){
        return EAI_NONAME;
    }
    int port = 0;
    if(service){
        port = service_port(service);
        if(port < 0){
            return EAI_SERVICE;
        }
    }
    int types[2] = {SOCK_STREAM, SOCK_DGRAM};
    int first = socktype == SOCK_DGRAM ? 1 : 0;
    int last = socktype == SOCK_STREAM ? 0 : 1;
    struct addrinfo *head = NULL, **tail = &head;
    for(int i = first; i <= last; i++){
        struct ai_node *n = calloc(1, sizeof(*n));
        if(!n){
            freeaddrinfo(head);
            return EAI_MEMORY;
        }
        n->sa.sin_family = AF_INET;
        n->sa.sin_port = htons((uint16_t)port);
        n->sa.sin_addr.s_addr = ip;
        n->ai.ai_family = AF_INET;
        n->ai.ai_socktype = types[i];
        n->ai.ai_protocol = types[i] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        n->ai.ai_addrlen = sizeof(n->sa);
        n->ai.ai_addr = (struct sockaddr *)&n->sa;
        *tail = &n->ai;
        tail = &n->ai.ai_next;
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res){
    while(res){
        struct addrinfo *next = res->ai_next;
        free(res);                           /* addrinfo e primul membru al nodului alocat */
        res = next;
    }
}

const char *gai_strerror(int code){
    switch(code){
    case EAI_BADFLAGS: return "optiuni invalide";
    case EAI_NONAME: return "nume necunoscut (fara DNS: doar adrese numerice si localhost)";
    case EAI_AGAIN: return "incearca din nou";
    case EAI_FAIL: return "eroare la rezolvare";
    case EAI_FAMILY: return "familie de adrese neacceptata";
    case EAI_SOCKTYPE: return "tip de socket neacceptat";
    case EAI_SERVICE: return "serviciu necunoscut";
    case EAI_MEMORY: return "fara memorie";
    default: return "eroare necunoscuta";
    }
}
