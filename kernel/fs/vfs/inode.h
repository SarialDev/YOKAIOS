#pragma once
#include "types.h"
#include "uapi/fs.h"
#include "uapi/syscall_nr.h"

struct Inode;
struct SuperBlock;
struct FileSystemType;
struct Dentry;

// O intrare de director, intoarsa de InodeOps::readdir
struct DirEntry {
    uint64_t ino;
    uint8_t type;                   // DT_*
    uint8_t name_len;
    char name[NAME_MAX + 1];
};

// Operatiile unui sistem de fisiere pe inode-uri. Toate sunt optionale (lipsa = -EPERM, sau -EINVAL pentru
// read/write/readlink). Namespace-ul e serializat de lock-ul VFS; read/write nu iau acest lock, deci fs-ul
// isi protejeaza singur datele fisierului. name nu e terminat cu 0 (lungimea in len)
struct InodeOps {
    // Inode-ul numit name din dir, -ENOENT daca lipseste. VFS ia singur referinta
    int (*lookup)(Inode* dir, const char* name, uint32_t len, Inode*& out);
    // Fisier / director / symlink nou (nlink = 1, size = lungimea tintei); VFS ia referinta
    int (*create)(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out);
    int (*mkdir)(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out);
    int (*symlink)(Inode* dir, const char* name, uint32_t len, const char* target, Inode*& out);
    // Scot intrarea; fs-ul scade nlink-ul lui victim (la 0 inode-ul se elibereaza in release_inode).
    // rmdir intoarce -ENOTEMPTY daca directorul nu e gol
    int (*unlink)(Inode* dir, const char* name, uint32_t len, Inode* victim);
    int (*rmdir)(Inode* dir, const char* name, uint32_t len, Inode* victim);
    // Pe offset explicit; fs-ul actualizeaza inode->size. Intorc bytes transferati sau -errno, read 0 = EOF
    int64_t (*read)(Inode* ino, uint64_t pos, void* buf, uint64_t len);
    int64_t (*write)(Inode* ino, uint64_t pos, const void* buf, uint64_t len);
    int (*truncate)(Inode* ino, uint64_t size);
    // Intrarea cu numarul index (fara "." si ".."): 1 = out completat, 0 = sfarsit, <0 = eroare
    int (*readdir)(Inode* dir, uint64_t index, DirEntry& out);
    // Tinta symlink-ului (fara terminator): lungimea copiata sau -errno
    int64_t (*readlink)(Inode* ino, char* buf, uint64_t len);
};

struct Inode {
    uint64_t ino;
    uint32_t mode;                  // S_IF* | permisiuni
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    Timespec atime;
    Timespec mtime;
    Timespec ctime;
    const InodeOps* ops;
    SuperBlock* sb;
    void* priv;
    uint32_t refs;                  // sub lock-ul VFS
};

struct SuperBlockOps {
    // Ultima referinta a unui inode a disparut: daca nlink == 0 fs-ul elibereaza datele si cheama inode_free,
    // altfel inode-ul ramane al fs-ului (cache-ul de dentry-uri nu il mai tine)
    void (*release_inode)(Inode* ino);
    // La umount, dupa ce toate dentry-urile au disparut: fs-ul elibereaza tot ce mai tine (inclusiv inode-urile)
    void (*kill)(SuperBlock* sb);
};

struct SuperBlock {
    FileSystemType* type;
    const SuperBlockOps* ops;
    void* priv;
    uint64_t dev;                   // identificator unic (st_dev)
    uint32_t flags;
    Inode* root_inode;              // completat de FileSystemType::mount, fara referinta
    Dentry* dentries;               // toate dentry-urile acestui sb, cu cele scoase din hash
};

// mount: sb vine alocat cu type si dev; fs-ul completeaza ops, priv si root_inode (inode_alloc). data poate fi nullptr
struct FileSystemType {
    const char* name;
    int (*mount)(FileSystemType* type, SuperBlock* sb, const char* dev_name, const char* data);
    FileSystemType* next;           // lista de inregistrare
};

// -EBUSY daca numele exista deja
int fs_register(FileSystemType* type);
FileSystemType* fs_find(const char* name);

// Inode nou, zero in afara de sb, ino, nlink = 1, ops, timpii; refs = 0 (referintele le ia VFS). nullptr fara memorie
Inode* inode_alloc(SuperBlock* sb, uint64_t ino, uint32_t mode, const InodeOps* ops);
void inode_free(Inode* ino);
void inode_get(Inode* ino);
void inode_put(Inode* ino);
// Cate inode-uri alocate prin inode_alloc exista (pentru teste: nu se pierd)
uint64_t inode_count();
// Ora curenta pentru atime/mtime/ctime
Timespec vfs_now();
