#ifndef _SYS_YINPUT_H
#define _SYS_YINPUT_H
#include <stdint.h>
/* Copie a kernel/include/uapi/yinput.h */
/* Intrarea compozitorului (T10.2): inputctl(INPUTCTL_GRAB) da un fd din care se citesc struct yk_input_event
 * (read blocant, poll POLLIN) cu toate evenimentele de tastatura si mouse. Cat timp fd-ul e deschis, captura e
 * exclusiva: terminalul nu mai primeste taste (un al doilea GRAB -> EBUSY); la close totul revine la terminal.
 * INPUTCTL_INJECT pune un eveniment in magistrala de input ca si cum ar veni de la hardware (teste; CAP_SYS_ADMIN). */
#define INPUTCTL_GRAB   1
#define INPUTCTL_INJECT 2

#define YK_EV_KEY       1
#define YK_EV_MOUSE     2

/* Butoane mouse (campul buttons) */
#define YK_BTN_LEFT     0x01
#define YK_BTN_RIGHT    0x02
#define YK_BTN_MIDDLE   0x04

/* Modificatori (campul modifiers) */
#define YK_MOD_SHIFT    0x01
#define YK_MOD_CTRL     0x02
#define YK_MOD_ALT      0x04
#define YK_MOD_CAPS     0x08

/* Cateva coduri de taste (KeyCode din kernel, independente de layout) */
#define YK_KEY_ESC        1
#define YK_KEY_BACKSPACE  14
#define YK_KEY_TAB        15
#define YK_KEY_ENTER      28
#define YK_KEY_SPACE      56
#define YK_KEY_F1         58
#define YK_KEY_HOME       72
#define YK_KEY_UP         73
#define YK_KEY_LEFT       75
#define YK_KEY_RIGHT      76
#define YK_KEY_END        77
#define YK_KEY_DOWN       78
#define YK_KEY_DELETE     81

struct yk_input_event {
    uint64_t time_ns;       /* CLOCK_MONOTONIC la primire */
    uint16_t type;          /* YK_EV_KEY / YK_EV_MOUSE */
    uint16_t keycode;       /* tasta: codul logic */
    uint8_t pressed;        /* tasta: 1 apasare (si autorepeat), 0 eliberare */
    uint8_t ascii;          /* tasta: caracterul (cu Shift/Caps), 0 daca nu e imprimabil */
    uint8_t modifiers;      /* YK_MOD_* */
    uint8_t buttons;        /* mouse: YK_BTN_* dupa acest pachet */
    int16_t dx, dy, dz;     /* mouse: deplasare relativa (dy pozitiv = in jos), roata */
    uint16_t pad;
};

int inputctl(int op, void *arg);
#endif
