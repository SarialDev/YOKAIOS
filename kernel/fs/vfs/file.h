#pragma once
#include "proc/file.h"
#include "fs/vfs/path.h"

// File-urile VFS: File (proc/file.h) cu inode, dentry, pos si flags. Operatiile lor sunt in vfs_file_ops
extern const FileOps vfs_file_ops;

// File nou peste p (preia referinta lui p.dentry; la eroare p ramane al apelantului). Lock-ul VFS luat.
// flags = O_* de la open. nullptr fara memorie
File* vfs_file_new(const Path& p, uint32_t flags);
// Din Inode in Stat
void vfs_fill_stat(const Inode* ino, Stat& st);
