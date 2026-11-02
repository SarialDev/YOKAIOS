#pragma once
/* Interfata terminalului (ioctl), identica cu cea de pe Linux x86-64: aceleasi numere si acelasi layout,
 * ca programele portate sa mearga fara schimbari. Folosita de kernel (drivers/tty.cpp) si, prin copie, de libc. */
#include "types.h"

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413

/* c_lflag */
#define TTY_ISIG    0x0001
#define TTY_ICANON  0x0002
#define TTY_ECHO    0x0008
#define TTY_ECHOE   0x0010

#define TTY_NCCS    19

struct KTermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[TTY_NCCS];
};

struct KWinsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};
