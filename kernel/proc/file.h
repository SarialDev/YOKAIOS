#pragma once
#include "types.h"

struct File;
struct Inode;
struct Dentry;

// Operatiile unui fisier deschis, pe buffere de kernel (copierea din/in user o face syscall-ul).
// len > 0; nonblock: fara asteptare, -EAGAIN daca nu se poate face nimic acum.
// Intorc cati bytes au fost transferati sau -errno; read 0 = sfarsit de fisier
struct FileOps {
    int64_t (*read)(File* f, void* buf, uint64_t len, bool nonblock);
    int64_t (*write)(File* f, const void* buf, uint64_t len, bool nonblock);
    void (*close)(File* f);         // la eliberarea ultimei referinte
    // Optionale (nullptr = nu se poate: -ESPIPE / -ENOTDIR)
    int64_t (*lseek)(File* f, int64_t off, int whence);
    int64_t (*getdents)(File* f, void* buf, uint64_t len);   // Dirent64 pe buffer de kernel
};

struct File {
    const FileOps* ops;
    void* priv;
    uint32_t refs;
    // Fisiere VFS: inode-ul, dentry-ul (cu referinta), pozitia (comuna dupa fork/dup), flags = O_*
    Inode* inode;
    Dentry* dentry;
    uint64_t pos;
    uint32_t flags;
};

// Fisier nou cu o referinta; nullptr fara memorie
File* file_alloc(const FileOps* ops, void* priv);
void file_get(File* f);
// Elibereaza o referinta; la ultima apeleaza ops->close si elibereaza File
void file_put(File* f);
// Consola (serial + VGA): read = EOF, write = kputc. Nu se elibereaza niciodata
File* console_file();

const int PROC_MAX_FD = 32;

// Tabelul de descriptori al unui proces: fd -> File*, cu cate o referinta pentru fiecare intrare
struct FdTable {
    File* fd[PROC_MAX_FD];
};

// Cel mai mic fd liber primeste referinta lui f; -EMFILE daca tabelul e plin (referinta ramane la apelant)
int fd_install(FdTable& t, File* f);
// File-ul de la fd, nullptr daca fd e invalid sau inchis
File* fd_get(const FdTable& t, int64_t fd);
int fd_close(FdTable& t, int64_t fd);
// Copie pentru fork: aceleasi File-uri, cu o referinta in plus fiecare
void fd_copy(FdTable& dst, const FdTable& src);
void fd_close_all(FdTable& t);
// stdin, stdout, stderr = consola
void fd_init_console(FdTable& t);
