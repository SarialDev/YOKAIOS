#pragma once
#include "types.h"
#include "proc/file.h"

// Socket-uri AF_UNIX (T10.2), doar SOCK_STREAM: protocolul client-compozitor. Un socket e un File (unix_ops), deci
// read/write/close/dup/fork/fcntl(O_NONBLOCK)/poll merg ca la pipe-uri. Numele (sun_path, sau abstract cu '\0' la
// inceput) stau intr-un tabel din kernel, NU in sistemul de fisiere: nu apare niciun fisier, numele se elibereaza
// la close (bind pe un nume ocupat de un socket viu -> EADDRINUSE). Fiecare sens are un buffer de UNIX_BUF octeti.
// Functiile intorc 0 / un rezultat >= 0 sau -errno.

const uint32_t UNIX_PATH_MAX = 108;
const uint32_t UNIX_BUF = 65536;

bool unix_is(const File* f);
// type = SOCK_STREAM (+ SOCK_NONBLOCK); altceva -> -ESOCKTNOSUPPORT / -EPROTONOSUPPORT
int unix_create(int type, int proto, File*& out);
int unix_pair(int type, int proto, File*& a, File*& b);
// path de len octeti (abstract daca path[0] == 0)
int unix_bind(File* f, const char* path, uint32_t len);
int unix_listen(File* f, int backlog);
int unix_accept(File* f, File*& out);
int unix_connect(File* f, const char* path, uint32_t len);
// flags: MSG_DONTWAIT (+ MSG_PEEK la recv)
int64_t unix_send(File* f, const void* buf, uint64_t len, int flags);
int64_t unix_recv(File* f, void* buf, uint64_t len, int flags);
int unix_shutdown(File* f, int how);
// Numele propriu / al partenerului (len = 0: fara nume)
int unix_name(File* f, bool peer, char* path, uint32_t& len);
int unix_getopt(File* f, int level, int opt, void* val, uint32_t* len);
int unix_setopt(File* f, int level, int opt, const void* val, uint32_t len);
// Socket-uri vii (pentru teste: nu se pierd)
uint64_t unix_count();
