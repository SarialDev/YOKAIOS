#pragma once
/* Socket-uri si poll/select: constantele si structurile de pe Linux x86-64, comune kernelului si libc-ului (T9.3).
 * Atentie: kernel/net/socket.cpp include antetele lwIP (care definesc alte valori pentru aceleasi nume) si NU include
 * acest fisier; traducerea o face el. */

#define AF_UNSPEC       0
#define AF_INET         2
#define PF_INET         AF_INET

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_NONBLOCK   04000       /* ca O_NONBLOCK: se poate combina cu tipul in socket() */

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define TCP_NODELAY     1

#define MSG_PEEK        0x02
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define POLLIN          0x001
#define POLLPRI         0x002
#define POLLOUT         0x004
#define POLLERR         0x008
#define POLLHUP         0x010
#define POLLNVAL        0x020

#define FD_SETSIZE      1024

/* Erori suplimentare (Linux) */
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETUNREACH     101
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115

#ifndef __ASSEMBLER__
struct k_sockaddr_in {              /* struct sockaddr_in de pe Linux */
    unsigned short sin_family;
    unsigned short sin_port;        /* network order */
    unsigned int sin_addr;          /* network order */
    unsigned char sin_zero[8];
};
struct k_pollfd {
    int fd;
    short events;
    short revents;
};
struct k_timeval {
    long tv_sec;
    long tv_usec;
};
#endif
