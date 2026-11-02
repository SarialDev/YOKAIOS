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
// Drepturile (fs/vfs/permission.h) se verifica cu identitatea procesului curent (cred_current; thread-urile de kernel
// sunt root): cautare (x) pe fiecare director din cale, r/w la open, w+x (si sticky) pe director la creare/stergere.
// La open/creare drepturile se verifica inaintea lui -EROFS. Nu exista page cache: datele merg direct la fs.
Mutex& vfs_mutex();
// Flag intern pentru vfs_open (peste O_*): deschidere pentru execve = drept x in loc de r, doar fisier obisnuit
const int VFS_O_EXEC = 1 << 30;
void vfs_init();

// Intoarce un File nou cu o referinta (de pus in FdTable cu fd_install). flags = O_*; mode doar la O_CREAT
int vfs_open(const Path& base, const char* path, int flags, uint32_t mode, File*& out);
int vfs_stat(const Path& base, const char* path, bool follow, Stat& st);
int vfs_mkdir(const Path& base, const char* path, uint32_t mode);
int vfs_rmdir(const Path& base, const char* path);
int vfs_unlink(const Path& base, const char* path);
// Muta / redenumeste (ca rename(2)): fisier peste fisier si director gol peste director gol inlocuiesc; -EXDEV intre
// sisteme de fisiere, -EPERM daca fs-ul nu stie rename
int vfs_rename(const Path& base, const char* oldpath, const char* newpath);
int vfs_symlink(const Path& base, const char* target, const char* linkpath);
// Lungimea tintei (fara terminator, trunchiata la size) sau -errno
int64_t vfs_readlink(const Path& base, const char* path, char* buf, uint64_t size);
// Schimba cwd-ul (referinta veche se elibereaza)
int vfs_chdir(Path& cwd, const char* path);
// Calea absoluta a lui cwd; lungimea cu terminator, -ERANGE, -ENOENT daca directorul a fost sters
int vfs_getcwd(const Path& cwd, char* buf, uint64_t size);
// Calea absoluta canonica a lui path (symlink-uri urmate), ca la getcwd: lungimea cu terminator, -ERANGE, -errno
int vfs_realpath(const Path& base, const char* path, char* buf, uint64_t size);
int vfs_mount(const Path& base, const char* dev_name, const char* target, const char* fstype, uint32_t flags,
              const char* data);
int vfs_umount(const Path& base, const char* target);

// chmod / chown / lchown (follow = false) / access (mode = F_OK sau R_OK|W_OK|X_OK, cu uid/gid-ul real)
int vfs_chmod(const Path& base, const char* path, uint32_t mode);
int vfs_chown(const Path& base, const char* path, uint32_t uid, uint32_t gid, bool follow);
int vfs_fchmod(File* f, uint32_t mode);
int vfs_fchown(File* f, uint32_t uid, uint32_t gid);
int vfs_access(const Path& base, const char* path, int mode);

// Referintele din Process::cwd
void vfs_path_copy(Path& dst, const Path& src);
void vfs_path_release(Path& p);
