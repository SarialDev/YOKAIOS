#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
/* socket-uri / interfete (T9.5), vezi net/if.h si net/route.h */
#define SIOCADDRT 0x890B
#define SIOCDELRT 0x890C
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFADDR 0x8915
#define SIOCSIFADDR 0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891B
#define SIOCSIFNETMASK 0x891C
#define SIOCGIFMTU 0x8921
#define SIOCGIFHWADDR 0x8927
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
int ioctl(int fd, unsigned long cmd, ...);
#endif
