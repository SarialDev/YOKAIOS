#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <stdint.h>
#include <sys/socket.h>
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;         /* network order */
    struct in_addr sin_addr;    /* network order */
    unsigned char sin_zero[8];
};
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t)0x0100007f)     /* 127.0.0.1 in network order (little-endian host) */
#define INET_ADDRSTRLEN 16
static inline uint16_t htons(uint16_t v){ return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v){ return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v){ return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v){ return __builtin_bswap32(v); }
#endif
