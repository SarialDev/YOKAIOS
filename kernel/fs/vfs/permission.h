#pragma once
#include "fs/vfs/inode.h"
#include "proc/cred.h"

// Drepturi Unix clasice (fara ACL): owner / group / other, rwx, sticky pe directoare, setuid/setgid la exec.
// root (euid 0) trece de toate, in afara de exec pe un fisier fara niciun bit x. Toate intorc 0 sau -errno
const int MAY_EXEC = 1;
const int MAY_WRITE = 2;
const int MAY_READ = 4;

// mask = MAY_*: -EACCES daca lipseste vreunul din drepturile cerute
int inode_permission(const Cred& c, const Inode* i, int mask);
// Nume nou in dir: w + x pe director
int may_create(const Cred& c, const Inode* dir);
// Scoaterea lui victim din dir: w + x pe director; cu sticky (S_ISVTX) doar proprietarul fisierului, al directorului
// sau root (altfel -EPERM)
int may_delete(const Cred& c, const Inode* dir, const Inode* victim);
// Dupa create/mkdir/symlink reusit: proprietar = euid, grup = egid (sau grupul directorului daca are S_ISGID, iar un
// director nou mosteneste S_ISGID). Doar pe fs-urile cu InodeOps::setattr (FAT/procfs/devfs isi pastreaza proprietarii)
void inode_init_owner(const Cred& c, const Inode* dir, Inode* ni);
// chmod / chown pe un inode (verifica drepturile, -EROFS, InodeOps::setattr; -EPERM daca fs-ul nu il are).
// uid/gid = CRED_NONE: neschimbat
int inode_chmod(Inode* i, uint32_t mode);
int inode_chown(Inode* i, uint32_t uid, uint32_t gid);
