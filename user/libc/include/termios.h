#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <sys/ioctl.h>
/* Acelasi layout ca struct termios din kernelul Linux (NCCS = 19): ioctl-urile TCGETS/TCSETS ale terminalului YokaiOS */
#define NCCS 19
typedef unsigned tcflag_t;
typedef unsigned char cc_t;
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
};
/* c_lflag: kernelul respecta ICANON (linie intreaga + editare in kernel), ECHO si ISIG (Ctrl-C = SIGINT, Ctrl-\\ = SIGQUIT
 * pentru grupul din prim-plan; programele care vor octetul 3 sterg ISIG) */
#define ISIG 0x0001
#define ICANON 0x0002
#define ECHO 0x0008
#define ECHOE 0x0010
/* c_cc */
#define VINTR 0
#define VQUIT 1
#define VEOF 4
#define VMIN 6
#define VTIME 5
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);
#endif
