#pragma once
/* Socket-uri si poll/select: constantele si structurile de pe Linux x86-64, comune kernelului si libc-ului (T9.3).
 * Atentie: kernel/net/socket.cpp include antetele lwIP (care definesc alte valori pentru aceleasi nume) si NU include
 * acest fisier; traducerea o face el. */

#define AF_UNSPEC       0
#define AF_UNIX         1           /* T10.2: doar SOCK_STREAM, nume in tabelul kernelului (net/unix.h) */
#define AF_INET         2
#define PF_INET         AF_INET

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3           /* doar cu IPPROTO_ICMP si CAP_NET (ping) */
#define SOCK_NONBLOCK   04000       /* ca O_NONBLOCK: se poate combina cu tipul in socket() */

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
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

/* ioctl pe socket-uri (T9.5), ca pe Linux: struct ifreq / struct rtentry */
#define SIOCADDRT       0x890B
#define SIOCDELRT       0x890C
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFBRDADDR  0x8919
#define SIOCGIFNETMASK  0x891B
#define SIOCSIFNETMASK  0x891C
#define SIOCGIFMTU      0x8921
#define SIOCGIFHWADDR   0x8927
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_RUNNING     0x40
#define IFF_MULTICAST   0x1000
#define RTF_UP          0x1
#define RTF_GATEWAY     0x2
#define IFNAMSIZ        16
#define ARPHRD_ETHER    1

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
struct k_ifreq {                    /* struct ifreq (40 de octeti) */
    char ifr_name[IFNAMSIZ];
    union {
        struct {                    /* struct sockaddr */
            unsigned short sa_family;
            unsigned char sa_data[14];
        } addr;
        short flags;
        int mtu;
        unsigned char pad[24];
    } u;
};
struct k_rtentry {                  /* struct rtentry (120 de octeti); doar ruta implicita, prin gateway */
    unsigned long rt_pad1;
    struct { unsigned short sa_family; unsigned char sa_data[14]; } rt_dst, rt_gateway, rt_genmask;
    unsigned short rt_flags;
    short rt_pad2;
    unsigned long rt_pad3;
    void* rt_pad4;
    short rt_metric;
    char* rt_dev;
    unsigned long rt_mtu, rt_window;
    unsigned short rt_irtt;
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
