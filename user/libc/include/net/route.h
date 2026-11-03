#ifndef _NET_ROUTE_H
#define _NET_ROUTE_H
#include <sys/socket.h>
/* Rute (T9.5): SIOCADDRT / SIOCDELRT accepta doar ruta implicita (dst 0.0.0.0, genmask 0.0.0.0) prin RTF_GATEWAY */
#define RTF_UP 0x1
#define RTF_GATEWAY 0x2
struct rtentry {
    unsigned long rt_pad1;
    struct sockaddr rt_dst;
    struct sockaddr rt_gateway;
    struct sockaddr rt_genmask;
    unsigned short rt_flags;
    short rt_pad2;
    unsigned long rt_pad3;
    void *rt_pad4;
    short rt_metric;
    char *rt_dev;
    unsigned long rt_mtu, rt_window;
    unsigned short rt_irtt;
};
#endif
