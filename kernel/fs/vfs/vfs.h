#pragma once
#include "fs/vfs/inode.h"
#include "fs/vfs/dentry.h"
#include "fs/vfs/path.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/file.h"
#include "sync/mutex.h"

// VFS: un singur lock (vfs_mutex) serializeaza namespace-ul (rezolvare de cai, cache de dentry-uri, mount-uri,
// contoarele de referinte). Functiile vfs_* de mai jos il iau singure si nu se apeleaza una din alta. Citirea si
// scrierea datelor nu il iau (le protejeaza sistemul de fisiere). Pozitia unui File comun (fork) se actualizeaza
// atomic, dar citiri concurente pe acelasi File pot citi aceleasi date.
// base = directorul de start al cailor relative (cwd); base.dentry == nullptr inseamna radacina.
// Nu exista permisiuni (T8.1) si nici page cache (T6.5): datele merg direct la sistemul de fisiere.
Mutex& vfs_mutex();
void vfs_init();

// Intoarce un File nou cu o referinta (de pus in FdTable cu fd_install). flags = O_*; mode doar la O_CREAT
int vfs_open(const Path& base, const char* path, int flags, uint32_t mode, File*& out);
int vfs_stat(const Path& base, const char* path, bool follow, Stat& st);
int vfs_mkdir(const Path& base, const char* path, uint32_t mode);
int vfs_rmdir(const Path& base, const char* path);
int vfs_unlink(const Path& base, const char* path);
int vfs_symlink(const Path& base, const char* target, const char* linkpath);
// Lungimea tintei (fara terminator, trunchiata la size) sau -errno
int64_t vfs_readlink(const Path& base, const char* path, char* buf, uint64_t size);
// Schimba cwd-ul (referinta veche se elibereaza)
int vfs_chdir(Path& cwd, const char* path);
// Calea absoluta a lui cwd; lungimea cu terminator, -ERANGE, -ENOENT daca directorul a fost sters
int vfs_getcwd(const Path& cwd, char* buf, uint64_t size);
int vfs_mount(const Path& base, const char* dev_name, const char* target, const char* fstype, uint32_t flags,
              const char* data);
int vfs_umount(const Path& base, const char* target);

// Referintele din Process::cwd
void vfs_path_copy(Path& dst, const Path& src);
void vfs_path_release(Path& p);
