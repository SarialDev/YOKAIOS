/* Ajutoare comune utilitarelor de retea (ping, wget, nc, ifconfig, netstat) - T9.5. Doar functii statice */
#ifndef NETUTIL_H
#define NETUTIL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* Microsecunde monotone */
static inline long long nu_now_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Adresa IPv4 (network order) a unui nume sau a unei adrese a.b.c.d; 0 la succes */
static inline int nu_resolve(const char *name, struct in_addr *out){
    if(inet_aton(name, out)){
        return 0;
    }
    struct hostent *h = gethostbyname(name);
    if(!h || h->h_addrtype != AF_INET || !h->h_addr_list[0]){
        return -1;
    }
    memcpy(out, h->h_addr_list[0], 4);
    return 0;
}

/* Numar zecimal fara semn (intreg intreg, fara semn, fara zero in fata gresit); false la format gresit */
static inline int nu_parse_uint(const char *s, unsigned long max, unsigned long *out){
    if(!*s){
        return 0;
    }
    unsigned long v = 0;
    for(const char *p = s; *p; p++){
        if(*p < '0' || *p > '9'){
            return 0;
        }
        v = v * 10 + (unsigned long)(*p - '0');
        if(v > max){
            return 0;
        }
    }
    *out = v;
    return 1;
}

#endif
