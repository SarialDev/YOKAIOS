#ifndef _PTY_H
#define _PTY_H
#include <termios.h>
/* Pseudo-terminal (T10.5): *amaster = capatul emulatorului, *aslave = terminalul programului (stdin/stdout/stderr ale copilului).
 * name, termp se ignora (nume nu exista: nu e /dev/pts); winp = dimensiunea initiala (NULL = 80x24). 0 sau -1 + errno */
int openpty(int *amaster, int *aslave, char *name, const struct termios *termp, const struct winsize *winp);
#endif
