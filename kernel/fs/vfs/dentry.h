#pragma once
#include "fs/vfs/inode.h"

// Toate functiile din acest header se apeleaza cu lock-ul VFS luat (vfs_mutex)

const uint32_t D_HASHED = 1;        // in tabela de hash (gasit de d_lookup)
const uint32_t D_DEAD = 2;          // numele a fost sters (unlink/rmdir); dentry-ul traieste cat e referit

// Un nume intr-un director. Dentry-urile sunt in cache cat timp numele exista; cele negative (inode == nullptr) si
// cele sterse dispar la ultima referinta. Fiecare copil tine o referinta la parinte
struct Dentry {
    Dentry* parent;                 // nullptr la radacina unui sistem de fisiere
    Inode* inode;                   // nullptr = dentry negativ
    SuperBlock* sb;
    Dentry* hash_next;
    Dentry* sb_next;
    Dentry* sb_prev;
    uint32_t refs;                  // include referintele copiilor (nchild)
    uint32_t nchild;
    uint32_t mounted;               // cate montari au acest director ca mount point
    uint32_t flags;
    uint32_t name_len;
    char name[];                    // terminat cu 0
};

// Radacina unui sb: o referinta, inode-ul primeste o referinta. nullptr fara memorie
Dentry* d_alloc_root(SuperBlock* sb, Inode* root);
// Gaseste (cache, apoi fs) numele in dir; 0 cu out referit, pozitiv sau negativ (out->inode == nullptr daca nu exista),
// sau -errno. dir trebuie sa fie pozitiv si director
int d_lookup(Dentry* dir, const char* name, uint32_t len, Dentry*& out);
// Leaga inode-ul (cu o referinta noua) de un dentry negativ
void d_instantiate(Dentry* d, Inode* ino);
// Scoate numele din cache (dupa unlink/rmdir); dentry-ul ramane valid pana la ultima referinta
void d_drop(Dentry* d);
void dentry_get(Dentry* d);
void dentry_put(Dentry* d);
// Referinte din afara cache-ului (fisiere, cwd, mount-uri) ale lui d
inline uint32_t d_external_refs(const Dentry* d){ return d->refs - d->nchild; }
// Elibereaza toate dentry-urile lui sb mai putin radacina (umount, dupa verificarea ca nimic nu e folosit)
void d_kill_sb(SuperBlock* sb);
// Cate dentry-uri exista (pentru teste: nu se pierd)
uint64_t dentry_count();
