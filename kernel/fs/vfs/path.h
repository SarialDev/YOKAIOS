#pragma once
#include "fs/vfs/dentry.h"

struct Mount;

// Un loc din arbore: mount-ul si dentry-ul. Tine o referinta la dentry (si prin ea mount-ul ramane montat)
struct Path {
    Mount* mnt;
    Dentry* dentry;
};

const unsigned LOOKUP_FOLLOW = 1;   // urmeaza symlink-ul din ultima componenta
const unsigned LOOKUP_PARENT = 2;   // se opreste inainte de ultima componenta (intoarsa in LastName)
const unsigned MAX_SYMLINKS = 8;    // symlink-uri urmate intr-o singura rezolvare; al 9-lea -> -ELOOP

struct LastName {
    char name[NAME_MAX + 1];
    uint32_t len;                   // 0 daca calea nu are componente ("/", "//")
    bool trailing_slash;
};

// Lock-ul VFS luat pentru toate.
// Rezolva name (absolut = de la radacina, altfel de la base; base.dentry == nullptr = radacina). Tratează "." si "..",
// slash-uri multiple, symlink-uri, mount point-uri. Cu LOOKUP_PARENT out e directorul care contine ultima componenta
// (care nu trebuie sa existe) si last o descrie. -ENOENT, -ENOTDIR, -ELOOP, -ENAMETOOLONG
int path_lookup(const Path& base, const char* name, unsigned flags, Path& out, LastName* last);
void path_get(const Path& p);
// Elibereaza referinta si goleste p
void path_put(Path& p);
// Calea absoluta a lui p in buf (cu terminator): lungimea incluzand terminatorul, -ERANGE, -ENOENT daca directorul a fost sters
int path_string(const Path& p, char* buf, uint64_t size);
