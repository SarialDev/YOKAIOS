#pragma once
#include "types.h"
#include "proc/file.h"
#include "uapi/tty.h"

// Pseudo-terminale (T10.5): o pereche master / slave. Slave-ul se poarta ca terminalul (disciplina de linie: canonic / brut,
// ECHO, ISIG, ICRNL, ONLCR; ioctl TCGETS/TCSETS/TIOCGPGRP/TIOCSPGRP/TIOCGWINSZ/TIOCSWINSZ), master-ul e capatul unui emulator:
// ce se scrie pe master ajunge ca "tastatura" slave-ului, ce scrie slave-ul se citeste de pe master.
// Master inchis -> slave citeste EOF si primeste SIGHUP (grupul din prim-plan); slave inchis -> master citeste EOF dupa ce
// goleste datele.

const int PTY_MAX = 8;

// master si slave primesc cate o referinta; 0 sau -errno (-ENOMEM, -ENOSPC daca toate cele PTY_MAX sunt folosite)
int pty_create(File*& master, File*& slave, const KWinsize* ws);
// f e capatul unui pty?
bool pty_is(const File* f);
// ioctl-urile terminalului pe un capat de pty
int64_t pty_ioctl(File* f, uint64_t cmd, uint64_t arg);
// Cate pty-uri exista acum (teste)
int pty_count();
