#pragma once
#include "types.h"
#include "proc/file.h"

// Socket-uri BSD (T9.3) peste socket-urile lwIP: un socket este un File (FileOps socket_ops) in tabelul de fd-uri.
// Interfata nu foloseste antete lwIP si valorile sunt cele de pe Linux (uapi/net.h); functiile intorc 0 / un rezultat >= 0
// sau -errno. lwIP ruleaza socket-ul mereu neblocant; asteptarea o face acest strat, in felii de 20 ms, ca un semnal sa
// intrerupa (-EINTR), cu SO_RCVTIMEO / SO_SNDTIMEO (-EAGAIN la expirare) si cu O_NONBLOCK / MSG_DONTWAIT.
// Apelurile blocheaza: se fac doar din task-uri (nu din task 0 / boot).

struct KSockAddr {
    uint32_t ip;                    // network order
    uint16_t port;                  // network order
};

// true daca f e un socket
bool sock_is(const File* f);

// AF_INET deja verificat de apelant; type = SOCK_STREAM / SOCK_DGRAM (+ SOCK_NONBLOCK), proto 0 sau IPPROTO_*
int sock_create(int type, int proto, File*& out);
int sock_bind(File* f, const KSockAddr& a);
int sock_listen(File* f, int backlog);
// Conexiune noua intr-un File separat; peer = adresa partenerului
int sock_accept(File* f, File*& out, KSockAddr& peer);
int sock_connect(File* f, const KSockAddr& a);
// to = nullptr: socket conectat. flags: MSG_DONTWAIT. len <= SOCK_IO_MAX (stream: apelantul repeta pana la capat)
int64_t sock_sendto(File* f, const void* buf, uint64_t len, int flags, const KSockAddr* to);
// flags: MSG_PEEK, MSG_DONTWAIT. from se completeaza la datagrame (*has_from = true). 0 = sfarsit de conexiune (TCP)
int64_t sock_recvfrom(File* f, void* buf, uint64_t len, int flags, KSockAddr* from, bool* has_from);
int sock_shutdown(File* f, int how);
int sock_setopt(File* f, int level, int opt, const void* val, uint32_t len);
// Valoarea intr-un buffer de cel mult 16 octeti; *len = lungimea ei
int sock_getopt(File* f, int level, int opt, void* val, uint32_t* len);
int sock_name(File* f, bool peer, KSockAddr& out);
bool sock_is_stream(const File* f);

const uint64_t SOCK_IO_MAX = 4096;

// poll: starea de acum a unui socket (biti Linux POLLIN/POLLOUT/POLLERR/POLLHUP)
uint32_t sock_poll_now(File* f);
// Asteapta pana la ms milisecunde (0 = doar verifica) evenimentele ev[i] pe socket-urile fs[i]; rev[i] = ce s-a intamplat.
// Intoarce cate socket-uri au evenimente
int sock_poll_wait(File* const* fs, const uint32_t* ev, uint32_t* rev, int n, int ms);
