#pragma once
#include "types.h"
#include "uapi/tty.h"

// Terminalul unic al sistemului (/dev/console, /dev/tty, fd 0-2 ale proceselor pornite de kernel).
// Intrarea vine de la tastatura (event bus), iesirea merge pe toate consolele (ecran + serial).
// Doua moduri, ca pe Linux: canonic (implicit: kernelul face ecoul, editarea cu Backspace si da citirii o linie intreaga)
// si brut (ICANON sters: fiecare octet e disponibil imediat, editarea o face programul; sagetile vin ca ESC [ A..D)

// Se aboneaza la event bus (inlocuieste orice abonat anterior)
void tty_init();
// Pana aici read() intoarce 0 (EOF): programele de test pornite inainte de init nu se blocheaza pe tastatura
void tty_activate();
bool tty_active();
// Blocheaza pana exista date (sau -EAGAIN cu nonblock). 0 = EOF (Ctrl-D pe linie goala, sau tty neactivat)
int64_t tty_read(void* buf, uint64_t len, bool nonblock);
int64_t tty_write(const void* buf, uint64_t len);
void tty_get_termios(KTermios& t);
void tty_set_termios(const KTermios& t);
void tty_get_winsize(KWinsize& w);
// Octeti primiti de la tastatura de la boot (pentru teste)
uint64_t tty_rx_count();
