#ifndef _NET_IF_H
#define _NET_IF_H
#include <sys/socket.h>
/* Interfete de retea (T9.5): ioctl SIOCGIF* / SIOCSIF* pe un socket, ca pe Linux. Adresele sunt struct sockaddr_in in ifr_addr */
#define IFNAMSIZ 16
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#define ARPHRD_ETHER 1
struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_mtu;
        char ifru_pad[24];
    } ifr_ifru;
};
#define ifr_addr ifr_ifru.ifru_addr
#define ifr_netmask ifr_ifru.ifru_addr
#define ifr_broadaddr ifr_ifru.ifru_addr
#define ifr_hwaddr ifr_ifru.ifru_hwaddr
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_mtu ifr_ifru.ifru_mtu
#endif
