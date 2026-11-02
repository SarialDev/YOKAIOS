#pragma once
#include "types.h"
#include "uapi/caps.h"

struct Cred;

// Capabilitatile unui proces (bitii CAP_* din uapi/caps.h). Mostenite la fork, reduse la exec dupa politica
// pachetului si cand identitatea trece de la root la un utilizator. Nu cresc niciodata; primul proces user (init,
// pornit de kernel) are CAP_ALL, la fel thread-urile de kernel
struct Caps {
    uint64_t permitted;             // plafon
    uint64_t effective;             // ce se verifica
};

extern const Caps caps_full;

// Capabilitatile procesului curent; caps_full pentru thread-urile de kernel
const Caps& caps_current();
// Are c capabilitatea cap? CAP_SYS_ADMIN include CAP_MOUNT
bool caps_has(const Caps& c, uint64_t cap);
// Procesul curent are capabilitatea cap?
bool capable(uint64_t cap);
// Poate c sa faca bind pe portul dat? (porturile < 1024 cer CAP_NET). Pentru stratul de socket-uri (inca inexistent)
bool caps_may_bind(const Caps& c, uint16_t port);
// Capabilitatile dupa o schimbare de identitate (setuid*, exec setuid), ca pe Linux: daca toate id-urile
// (uid/euid/suid) au devenit nenule, totul se pierde; euid 0 -> nenul goleste efectivul; nenul -> 0 il reface din permitted
void caps_on_setid(Caps& c, const Cred& before, const Cred& after);
// Reduce c la mask (permitted si effective)
void caps_limit(Caps& c, uint64_t mask);

// Politica de exec: fisierul executabil (cale rezolvata, de la base) apare in /etc/exec.caps sau
// /var/lib/ypkg/exec.caps cu un set de capabilitati? Linii "<cale absoluta> <cap,cap|none> [pachet]", '#' = comentariu.
// Daca da, found = true si mask e setul permis. Fisierele lipsa = nicio politica
struct Path;
void caps_exec_policy(const Path& base, const char* path, bool& found, uint64_t& mask);
// Parseaza o lista "rt,net" / "none" / "all" / "0x28"; false la nume necunoscut
bool caps_parse(const char* s, uint64_t& out);
// Numele capabilitatilor din mask, separate prin ',' ("none" daca e gol), in buf
void caps_format(uint64_t mask, char* buf, uint64_t size);

// La execve: bitmap-ul ioperm al procesului curent dispare
void caps_exec_reset_io();

// Syscall-uri (caps.cpp)
int64_t sys_capget(uint64_t pid, uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_capset(uint64_t in, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy, uint64_t param, uint64_t, uint64_t, uint64_t);
int64_t sys_sched_getscheduler(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_ioperm(uint64_t from, uint64_t num, uint64_t on, uint64_t, uint64_t, uint64_t);
