#pragma once
#include "types.h"

struct Inode;

// Identitatea unui proces (ca pe Linux, fara capabilitati: euid 0 = root ocoleste verificarile de fisiere, in afara de
// exec fara niciun bit x). uid/gid = reale, euid/egid = efective (cele verificate), suid/sgid = salvate (setuid inapoi)
const uint32_t CRED_NGROUPS_MAX = 32;
const uint32_t CRED_NONE = 0xFFFFFFFFu;          // -1 la chown/setreuid: "nu schimba"

struct Cred {
    uint32_t uid, gid;
    uint32_t euid, egid;
    uint32_t suid, sgid;
    uint32_t ngroups;
    uint32_t groups[CRED_NGROUPS_MAX];         // grupuri suplimentare
};

// root (0/0, fara grupuri suplimentare): procesele pornite de kernel si thread-urile de kernel
extern const Cred cred_root;
// Identitatea procesului curent; cred_root pentru thread-urile de kernel
const Cred& cred_current();
bool cred_is_root(const Cred& c);
// egid sau unul din grupurile suplimentare
bool cred_in_group(const Cred& c, uint32_t gid);
// Poate c sa trimita semnale procesului cu identitatea t? (root, sau uid/euid-ul lui c = uid/suid-ul lui t)
bool cred_may_signal(const Cred& c, const Cred& t);
// La execve, dupa punctul fara intoarcere: bitii S_ISUID / S_ISGID (cu S_IXGRP) ai fisierului schimba euid/egid,
// daca montarea nu are MS_NOSUID. suid/sgid devin cele efective
void cred_exec(Cred& c, uint32_t mode, uint32_t uid, uint32_t gid, bool nosuid);
// umask-ul procesului curent; 0 pentru thread-urile de kernel
uint32_t cred_umask();

// Syscall-uri (cred.cpp), numerele Linux, legate in table.cpp
int64_t sys_getuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_geteuid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getgid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getegid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setuid(uint64_t uid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setgid(uint64_t gid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setreuid(uint64_t ruid, uint64_t euid, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setregid(uint64_t rgid, uint64_t egid, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getgroups(uint64_t size, uint64_t list, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setgroups(uint64_t size, uint64_t list, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_umask(uint64_t mask, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
