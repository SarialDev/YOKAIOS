#include "tests/test_vfs.h"
#include "uapi/syscall_nr.h"
#include "fs/vfs/vfs.h"
#include "core/panic.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "time/clock.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_vfs:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_vfs:%d: %s", __LINE__, #cond); } } while(0)

// ---- memfs: tot in memorie, fara locking propriu (testele ruleaza intr-un singur task) ----

const uint64_t MEM_MAX_FILE = 1 << 20;

struct MemNode;

struct MemEntry {
    MemEntry* next;
    MemNode* node;
    uint8_t len;
    char name[NAME_MAX];
};

struct MemNode {
    Inode* ino;
    MemNode* all_next;
    uint8_t* data;
    uint64_t cap;
    MemEntry* ents;
};

struct MemFs {
    uint64_t next_ino;
    MemNode* all;
};

extern const InodeOps mem_ops;
extern const SuperBlockOps mem_sb_ops;

MemNode* node_of(Inode* i){
    return (MemNode*)i->priv;
}

MemNode* mem_new(SuperBlock* sb, uint32_t mode){
    MemFs* fs = (MemFs*)sb->priv;
    MemNode* n = (MemNode*)kzalloc(sizeof(MemNode));
    Inode* i = n ? inode_alloc(sb, fs->next_ino++, mode, &mem_ops) : nullptr;
    if(!i){
        kfree(n);
        return nullptr;
    }
    i->priv = n;
    if(S_ISDIR(mode)){
        i->nlink = 2;
    }
    n->ino = i;
    n->all_next = fs->all;
    fs->all = n;
    return n;
}

void mem_destroy(MemNode* n){
    while(n->ents){
        MemEntry* e = n->ents;
        n->ents = e->next;
        kfree(e);
    }
    kfree(n->data);
    inode_free(n->ino);
    kfree(n);
}

MemEntry** mem_find(MemNode* dir, const char* name, uint32_t len){
    MemEntry** pp = &dir->ents;
    while(*pp && !((*pp)->len == len && memcmp((*pp)->name, name, len) == 0)){
        pp = &(*pp)->next;
    }
    return pp;
}

int mem_add(Inode* dir, const char* name, uint32_t len, MemNode* n, Inode*& out){
    MemEntry* e = (MemEntry*)kzalloc(sizeof(MemEntry));
    if(!e){
        return -ENOMEM;
    }
    e->node = n;
    e->len = (uint8_t)len;
    memcpy(e->name, name, len);
    MemEntry** pp = &node_of(dir)->ents;
    while(*pp){
        pp = &(*pp)->next;
    }
    *pp = e;
    out = n->ino;
    return 0;
}

int mem_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    MemEntry* e = *mem_find(node_of(dir), name, len);
    if(!e){
        return -ENOENT;
    }
    out = e->node->ino;
    return 0;
}

int mem_make(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out){
    MemNode* n = mem_new(dir->sb, mode);
    if(!n){
        return -ENOMEM;
    }
    int err = mem_add(dir, name, len, n, out);
    if(err){
        n->ino->nlink = 0;
        mem_destroy(n);
        return err;
    }
    if(S_ISDIR(mode)){
        dir->nlink++;
    }
    return 0;
}

int mem_symlink(Inode* dir, const char* name, uint32_t len, const char* target, Inode*& out){
    int err = mem_make(dir, name, len, S_IFLNK | 0777, out);
    if(err){
        return err;
    }
    uint64_t n = 0;
    while(target[n]){
        n++;
    }
    MemNode* node = node_of(out);
    node->data = (uint8_t*)kmalloc(n);
    memcpy(node->data, target, n);
    node->cap = n;
    out->size = n;
    return 0;
}

int mem_remove(Inode* dir, const char* name, uint32_t len, Inode* victim, bool is_dir){
    MemEntry** pp = mem_find(node_of(dir), name, len);
    MemEntry* e = *pp;
    if(!e || e->node->ino != victim){
        return -ENOENT;
    }
    if(is_dir){
        if(e->node->ents){
            return -ENOTEMPTY;
        }
        dir->nlink--;
        victim->nlink = 0;
    } else {
        victim->nlink--;
    }
    *pp = e->next;
    kfree(e);
    return 0;
}

int mem_unlink(Inode* dir, const char* name, uint32_t len, Inode* victim){
    return mem_remove(dir, name, len, victim, false);
}

int mem_rmdir(Inode* dir, const char* name, uint32_t len, Inode* victim){
    return mem_remove(dir, name, len, victim, true);
}

int mem_resize(Inode* i, uint64_t size){
    MemNode* n = node_of(i);
    if(size > MEM_MAX_FILE){
        return -EFBIG;
    }
    if(size > n->cap){
        uint8_t* d = (uint8_t*)krealloc(n->data, size);
        if(!d){
            return -ENOMEM;
        }
        memset(d + n->cap, 0, size - n->cap);
        n->data = d;
        n->cap = size;
    }
    if(size > i->size){
        memset(n->data + i->size, 0, size - i->size);
    }
    i->size = size;
    return 0;
}

int64_t mem_read(Inode* i, uint64_t pos, void* buf, uint64_t len){
    if(pos >= i->size){
        return 0;
    }
    uint64_t n = i->size - pos < len ? i->size - pos : len;
    memcpy(buf, node_of(i)->data + pos, n);
    return (int64_t)n;
}

int64_t mem_write(Inode* i, uint64_t pos, const void* buf, uint64_t len){
    if(pos + len > i->size){
        int err = mem_resize(i, pos + len);
        if(err){
            return err;
        }
    }
    memcpy(node_of(i)->data + pos, buf, len);
    return (int64_t)len;
}

int mem_truncate(Inode* i, uint64_t size){
    return mem_resize(i, size);
}

int mem_readdir(Inode* dir, uint64_t index, DirEntry& out){
    MemEntry* e = node_of(dir)->ents;
    while(e && index--){
        e = e->next;
    }
    if(!e){
        return 0;
    }
    out.ino = e->node->ino->ino;
    out.type = S_ISDIR(e->node->ino->mode) ? DT_DIR : S_ISLNK(e->node->ino->mode) ? DT_LNK : DT_REG;
    out.name_len = e->len;
    memcpy(out.name, e->name, e->len);
    out.name[e->len] = 0;
    return 1;
}

int64_t mem_readlink(Inode* i, char* buf, uint64_t len){
    uint64_t n = i->size < len ? i->size : len;
    memcpy(buf, node_of(i)->data, n);
    return (int64_t)n;
}

const InodeOps mem_ops = {mem_lookup, mem_make, mem_make, mem_symlink, mem_unlink, mem_rmdir, mem_read, mem_write,
                          mem_truncate, mem_readdir, mem_readlink};

void mem_release(Inode* i){
    if(i->nlink != 0){
        return;                                     // ramane al fs-ului pana la kill
    }
    MemFs* fs = (MemFs*)i->sb->priv;
    MemNode* n = node_of(i);
    for(MemNode** pp = &fs->all; *pp; pp = &(*pp)->all_next){
        if(*pp == n){
            *pp = n->all_next;
            break;
        }
    }
    mem_destroy(n);
}

void mem_kill(SuperBlock* sb){
    MemFs* fs = (MemFs*)sb->priv;
    while(fs->all){
        MemNode* n = fs->all;
        fs->all = n->all_next;
        mem_destroy(n);
    }
    kfree(fs);
}

const SuperBlockOps mem_sb_ops = {mem_release, mem_kill};

int mem_mount(FileSystemType*, SuperBlock* sb, const char*, const char*){
    MemFs* fs = (MemFs*)kzalloc(sizeof(MemFs));
    if(!fs){
        return -ENOMEM;
    }
    fs->next_ino = 1;
    sb->ops = &mem_sb_ops;
    sb->priv = fs;
    MemNode* root = mem_new(sb, S_IFDIR | 0755);
    if(!root){
        kfree(fs);
        return -ENOMEM;
    }
    sb->root_inode = root->ino;
    return 0;
}

FileSystemType memfs_type = {"memfs", mem_mount, nullptr};

// ---- ajutoare de test ----

Path no_base = {};

int open_err(const char* path, int flags = O_RDONLY){
    File* f;
    int err = vfs_open(no_base, path, flags, 0644, f);
    if(!err){
        file_put(f);
    }
    return err;
}

File* open_ok(const char* path, int flags = O_RDONLY){
    File* f = nullptr;
    int err = vfs_open(no_base, path, flags, 0644, f);
    if(err){
        panic("test_vfs: open(%s) = %d", path, err);
    }
    return f;
}

uint64_t slen(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

bool same(const char* a, const char* b){
    return slen(a) == slen(b) && memcmp(a, b, slen(a)) == 0;
}

int64_t put(File* f, const char* s){
    return f->ops->write(f, s, slen(s), false);
}

// Citeste tot ce urmeaza de la pozitia curenta (max cap-1 bytes) ca sir
int64_t get(File* f, char* buf, uint64_t cap){
    int64_t n = f->ops->read(f, buf, cap - 1, false);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

void expect_content(const char* path, const char* want){
    File* f = open_ok(path);
    char buf[64];
    EQ(get(f, buf, sizeof(buf)), (int64_t)slen(want));
    CHECK(same(buf, want));
    file_put(f);
}

Stat stat_ok(const char* path, bool follow = true){
    Stat st;
    int err = vfs_stat(no_base, path, follow, st);
    if(err){
        panic("test_vfs: stat(%s) = %d", path, err);
    }
    return st;
}

int stat_err(const char* path, bool follow = true){
    Stat st;
    return vfs_stat(no_base, path, follow, st);
}

void make_dir(const char* path){
    EQ(vfs_mkdir(no_base, path, 0755), 0);
}

void make_file(const char* path, const char* content){
    File* f = open_ok(path, O_CREAT | O_EXCL | O_WRONLY);
    EQ(put(f, content), (int64_t)slen(content));
    file_put(f);
}

// ---- teste ----

void test_files(){
    EQ(fs_register(&memfs_type), 0);
    EQ(fs_register(&memfs_type), -EBUSY);
    CHECK(fs_find("memfs") == &memfs_type);
    CHECK(!fs_find("nu-exista"));
    EQ(open_err("/x"), -ENOENT);                           // nimic montat
    EQ(vfs_mount(no_base, "", "/nope", "memfs", 0, nullptr), -ENOENT);
    EQ(vfs_mount(no_base, "", "/", "nu-exista", 0, nullptr), -ENODEV);
    EQ(vfs_mount(no_base, "", "/", "memfs", 0, nullptr), 0);

    make_dir("/a");
    make_dir("/a/b");
    make_dir("/tmp");
    make_dir("/mnt");
    make_file("/a/hello", "hello world");

    File* f = open_ok("/a/hello", O_RDWR);
    char buf[64];
    EQ(get(f, buf, sizeof(buf)), 11);
    CHECK(same(buf, "hello world"));
    EQ(get(f, buf, sizeof(buf)), 0);                       // EOF
    EQ(f->ops->lseek(f, 6, SEEK_SET), 6);
    EQ(get(f, buf, sizeof(buf)), 5);
    CHECK(same(buf, "world"));
    EQ(f->ops->lseek(f, -5, SEEK_END), 6);
    EQ(f->ops->lseek(f, -3, SEEK_CUR), 3);
    EQ(f->ops->lseek(f, -100, SEEK_CUR), -EINVAL);
    EQ(f->ops->lseek(f, 0, 9), -EINVAL);
    EQ(put(f, "LO W"), 4);                                 // suprascrie de la 3
    EQ(f->ops->lseek(f, 0, SEEK_SET), 0);
    EQ(get(f, buf, sizeof(buf)), 11);
    CHECK(same(buf, "helLO World"));
    // Dupa sfarsit: golul se umple cu zerouri
    EQ(f->ops->lseek(f, 14, SEEK_SET), 14);
    EQ(put(f, "!"), 1);
    EQ(f->ops->lseek(f, 11, SEEK_SET), 11);
    EQ(f->ops->read(f, buf, 4, false), 4);
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == '!');
    file_put(f);

    Stat st = stat_ok("/a/hello");
    CHECK(S_ISREG(st.st_mode));
    EQ(st.st_size, 15);
    EQ(st.st_nlink, 1);
    EQ(st.st_blocks, 1);
    CHECK(st.st_ino != 0);
    Stat sd = stat_ok("/a");
    CHECK(S_ISDIR(sd.st_mode));
    EQ(sd.st_nlink, 3);                                    // ., intrarea din /, ..-ul lui b
    EQ(sd.st_dev, st.st_dev);

    // Acces: RDONLY nu scrie, WRONLY nu citeste, directorul nu se citeste
    f = open_ok("/a/hello", O_RDONLY);
    EQ(put(f, "x"), -EBADF);
    file_put(f);
    f = open_ok("/a/hello", O_WRONLY);
    EQ(f->ops->read(f, buf, 4, false), -EBADF);
    file_put(f);
    f = open_ok("/a", O_RDONLY);
    EQ(f->ops->read(f, buf, 4, false), -EISDIR);
    file_put(f);

    // O_TRUNC, O_APPEND
    f = open_ok("/a/hello", O_WRONLY | O_TRUNC);
    file_put(f);
    EQ(stat_ok("/a/hello").st_size, 0);
    make_file("/a/log", "abc");
    f = open_ok("/a/log", O_WRONLY | O_APPEND);
    EQ(f->ops->lseek(f, 0, SEEK_SET), 0);
    EQ(put(f, "def"), 3);
    file_put(f);
    expect_content("/a/log", "abcdef");
    EQ(vfs_unlink(no_base, "/a/log"), 0);

    // Fisier sters cat timp e deschis: datele raman pana la close, apoi inode-ul dispare
    make_file("/tmp/keep", "keep");
    uint64_t inodes = inode_count();
    uint64_t dentries = dentry_count();
    f = open_ok("/tmp/keep");
    EQ(vfs_unlink(no_base, "/tmp/keep"), 0);
    EQ(open_err("/tmp/keep"), -ENOENT);
    EQ(stat_ok("/tmp").st_nlink, 2);
    EQ(f->inode->nlink, 0);
    EQ(get(f, buf, sizeof(buf)), 4);
    CHECK(same(buf, "keep"));
    EQ(inode_count(), inodes);
    file_put(f);
    EQ(inode_count(), inodes - 1);
    EQ(dentry_count(), dentries - 1);
}

void test_paths(){
    char buf[64];
    expect_content("//a///b/./../hello", "");
    EQ(open_err("/a/b/../../a/hello"), 0);
    EQ(open_err("/../../a/hello"), 0);                     // .. de la radacina ramane la radacina
    EQ(open_err("/a/"), 0);
    EQ(open_err("/a/hello/"), -ENOTDIR);
    EQ(open_err("/a/hello/x"), -ENOTDIR);
    EQ(open_err("/a/nope"), -ENOENT);
    EQ(open_err("/nope/x"), -ENOENT);
    EQ(open_err(""), -ENOENT);
    EQ(open_err("/a/hello", O_DIRECTORY), -ENOTDIR);
    EQ(open_err("/a", O_WRONLY), -EISDIR);
    EQ(open_err("/a", O_RDWR | O_CREAT), -EISDIR);
    EQ(open_err("/a/hello", O_CREAT | O_EXCL), -EEXIST);
    EQ(open_err("/a/.", O_CREAT | O_EXCL), -EEXIST);
    EQ(open_err("/a/nou/", O_CREAT | O_WRONLY), -EISDIR);
    EQ(open_err("/a/nope/x", O_CREAT | O_WRONLY), -ENOENT);
    EQ(open_err("/a/hello/x", O_CREAT | O_WRONLY), -ENOTDIR);
    EQ(open_err("/", O_RDONLY | O_DIRECTORY), 0);
    char longname[300];
    memset(longname, 'x', sizeof(longname));
    longname[256] = 0;                                     // componenta de 256 > NAME_MAX
    EQ(open_err(longname), -ENAMETOOLONG);
    longname[0] = '/';
    longname[256] = 'x';
    longname[257] = 0;
    EQ(open_err(longname), -ENAMETOOLONG);
    longname[256] = 0;                                     // 255 = NAME_MAX: ajunge la fs
    EQ(open_err(longname), -ENOENT);

    EQ(vfs_mkdir(no_base, "/a", 0755), -EEXIST);
    EQ(vfs_mkdir(no_base, "/", 0755), -EEXIST);
    EQ(vfs_mkdir(no_base, "/a/.", 0755), -EEXIST);
    EQ(vfs_mkdir(no_base, "/nope/x", 0755), -ENOENT);
    EQ(vfs_mkdir(no_base, "/a/hello/x", 0755), -ENOTDIR);
    EQ(vfs_rmdir(no_base, "/a"), -ENOTEMPTY);
    EQ(vfs_rmdir(no_base, "/a/hello"), -ENOTDIR);
    EQ(vfs_rmdir(no_base, "/a/b/."), -EINVAL);
    EQ(vfs_rmdir(no_base, "/a/b/.."), -ENOTEMPTY);
    EQ(vfs_rmdir(no_base, "/"), -EBUSY);
    EQ(vfs_rmdir(no_base, "/nope"), -ENOENT);
    EQ(vfs_unlink(no_base, "/a"), -EISDIR);
    EQ(vfs_unlink(no_base, "/"), -EISDIR);
    EQ(vfs_unlink(no_base, "/nope"), -ENOENT);
    EQ(vfs_unlink(no_base, "/a/hello/"), -ENOTDIR);

    make_dir("/tmp/x");
    EQ(stat_ok("/tmp").st_nlink, 3);
    EQ(vfs_rmdir(no_base, "/tmp/x"), 0);
    EQ(stat_ok("/tmp").st_nlink, 2);
    EQ(stat_err("/tmp/x"), -ENOENT);

    // cwd: cai relative, getcwd, chdir
    Path cwd = {};
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), 2);              // "/" + terminator
    CHECK(same(buf, "/"));
    EQ(vfs_chdir(cwd, "/a"), 0);
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), 3);
    CHECK(same(buf, "/a"));
    EQ(vfs_getcwd(cwd, buf, 2), -ERANGE);
    File* f;
    EQ(vfs_open(cwd, "hello", O_RDONLY, 0, f), 0);
    file_put(f);
    EQ(vfs_open(cwd, "b/../hello", O_RDONLY, 0, f), 0);
    file_put(f);
    EQ(vfs_open(cwd, "../a/hello", O_RDONLY, 0, f), 0);
    file_put(f);
    EQ(vfs_open(cwd, "./b/../../a/hello", O_RDONLY, 0, f), 0);
    file_put(f);
    EQ(vfs_open(cwd, ".", O_RDONLY | O_DIRECTORY, 0, f), 0);
    file_put(f);
    EQ(vfs_open(cwd, "nou", O_CREAT | O_WRONLY, 0600, f), 0);
    EQ(put(f, "rel"), 3);
    file_put(f);
    expect_content("/a/nou", "rel");
    EQ(vfs_unlink(cwd, "nou"), 0);
    EQ(vfs_chdir(cwd, "hello"), -ENOTDIR);
    EQ(vfs_chdir(cwd, "nope"), -ENOENT);
    EQ(vfs_chdir(cwd, "b"), 0);
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), 5);
    CHECK(same(buf, "/a/b"));
    EQ(vfs_chdir(cwd, "../.."), 0);
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), 2);
    EQ(vfs_chdir(cwd, "/../.."), 0);

    // cwd intr-un director sters: getcwd da ENOENT, calea relativa merge in continuare
    make_dir("/gone");
    EQ(vfs_chdir(cwd, "/gone"), 0);
    EQ(vfs_rmdir(no_base, "/gone"), 0);
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), -ENOENT);
    EQ(vfs_open(cwd, "../a/hello", O_RDONLY, 0, f), 0);    // .. din directorul sters ajunge in parinte
    file_put(f);
    EQ(stat_ok("/").st_nlink, 5);
    EQ(vfs_chdir(cwd, "/"), 0);
    vfs_path_release(cwd);
    CHECK(!cwd.dentry);
}

void test_symlinks(){
    char buf[64];
    EQ(vfs_symlink(no_base, "/a/hello", "/s1"), 0);
    EQ(vfs_symlink(no_base, "s1", "/s2"), 0);              // relativ la directorul linkului
    EQ(vfs_symlink(no_base, "../hello", "/a/b/up"), 0);
    EQ(vfs_symlink(no_base, "/a", "/dirlink"), 0);
    EQ(vfs_symlink(no_base, "a", "/s1"), -EEXIST);
    EQ(vfs_symlink(no_base, "", "/s9"), -ENOENT);
    File* hf = open_ok("/a/hello", O_WRONLY | O_TRUNC);
    EQ(put(hf, "hello world"), 11);
    file_put(hf);
    expect_content("/s1", "hello world");
    expect_content("/s2", "hello world");                  // doua symlink-uri in lant
    expect_content("/a/b/up", "hello world");
    expect_content("/dirlink/hello", "hello world");       // symlink pe mijlocul caii
    expect_content("/dirlink/b/up", "hello world");
    expect_content("/dirlink/../s1", "hello world");

    Stat l = stat_ok("/s1", false);
    CHECK(S_ISLNK(l.st_mode));
    EQ(l.st_size, 8);
    Stat t = stat_ok("/s1", true);
    CHECK(S_ISREG(t.st_mode));
    EQ(t.st_size, 11);
    CHECK(S_ISDIR(stat_ok("/dirlink/").st_mode));
    EQ(stat_err("/s1/"), -ENOTDIR);

    EQ(vfs_readlink(no_base, "/s1", buf, sizeof(buf)), 8);
    CHECK(memcmp(buf, "/a/hello", 8) == 0);
    EQ(vfs_readlink(no_base, "/s1", buf, 3), 3);
    EQ(vfs_readlink(no_base, "/a/hello", buf, sizeof(buf)), -EINVAL);
    EQ(open_err("/s1", O_RDONLY | O_NOFOLLOW), -ELOOP);
    EQ(open_err("/s1", O_CREAT | O_EXCL | O_WRONLY), -EEXIST);
    EQ(vfs_unlink(no_base, "/dirlink/"), -ENOTDIR);        // calea cu slash final nu e un nume de fisier

    // Limita de 8 symlink-uri urmate: c1..c8 merg, d1..d9 nu
    for(int i = 1; i <= 7; i++){
        char name[8] = {'/', 'c', (char)('0' + i), 0};
        char tgt[4] = {'c', (char)('0' + i + 1), 0};
        EQ(vfs_symlink(no_base, tgt, name), 0);
    }
    EQ(vfs_symlink(no_base, "/a/hello", "/c8"), 0);
    expect_content("/c1", "hello world");
    for(int i = 1; i <= 8; i++){
        char name[8] = {'/', 'd', (char)('0' + i), 0};
        char tgt[4] = {'d', (char)('0' + i + 1), 0};
        EQ(vfs_symlink(no_base, tgt, name), 0);
    }
    EQ(vfs_symlink(no_base, "/a/hello", "/d9"), 0);
    EQ(open_err("/d1"), -ELOOP);
    expect_content("/d2", "hello world");
    EQ(vfs_symlink(no_base, "loop", "/loop"), 0);
    EQ(open_err("/loop"), -ELOOP);
    EQ(stat_err("/loop"), -ELOOP);
    CHECK(S_ISLNK(stat_ok("/loop", false).st_mode));
    EQ(vfs_symlink(no_base, "/nope", "/dangling"), 0);
    EQ(open_err("/dangling"), -ENOENT);
    EQ(stat_err("/dangling"), -ENOENT);

    // unlink sterge linkul, nu tinta
    EQ(vfs_unlink(no_base, "/s1"), 0);
    EQ(open_err("/s2"), -ENOENT);                          // s2 arata acum spre un link lipsa
    expect_content("/a/hello", "hello world");
    const char* links[] = {"/s2", "/a/b/up", "/dirlink", "/c1", "/c2", "/c3", "/c4", "/c5", "/c6", "/c7", "/c8",
                           "/d1", "/d2", "/d3", "/d4", "/d5", "/d6", "/d7", "/d8", "/d9", "/loop", "/dangling"};
    for(const char* l2 : links){
        EQ(vfs_unlink(no_base, l2), 0);
    }
}

void test_mounts(){
    uint64_t dentries = dentry_count();
    uint64_t inodes = inode_count();
    uint64_t mounts = mount_count();
    uint64_t root_dev = stat_ok("/").st_dev;
    char buf[64];

    EQ(vfs_mount(no_base, "", "/a/hello", "memfs", 0, nullptr), -ENOTDIR);
    EQ(vfs_mount(no_base, "", "/nope", "memfs", 0, nullptr), -ENOENT);
    EQ(vfs_umount(no_base, "/a"), -EINVAL);                // nu e radacina unui mount
    EQ(vfs_umount(no_base, "/"), -EBUSY);
    EQ(vfs_mount(no_base, "dev0", "/mnt", "memfs", 0, nullptr), 0);
    EQ(mount_count(), mounts + 1);
    CHECK(stat_ok("/mnt").st_dev != root_dev);
    EQ(stat_ok("/mnt/..").st_dev, root_dev);
    CHECK(S_ISDIR(stat_ok("/mnt/../a").st_mode));
    EQ(vfs_rmdir(no_base, "/mnt"), -EBUSY);                // mount point
    EQ(vfs_unlink(no_base, "/mnt"), -EISDIR);
    EQ(open_err("/mnt/f"), -ENOENT);                       // noul fs e gol
    make_file("/mnt/f", "in mnt");
    make_dir("/mnt/sub");
    EQ(stat_ok("/mnt/f").st_dev, stat_ok("/mnt").st_dev);
    CHECK(stat_ok("/mnt/f").st_ino != 0);

    Path cwd = {};
    EQ(vfs_chdir(cwd, "/mnt/sub"), 0);
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), 9);
    CHECK(same(buf, "/mnt/sub"));
    File* f;
    EQ(vfs_open(cwd, "../f", O_RDONLY, 0, f), 0);
    file_put(f);
    EQ(vfs_open(cwd, "../../a/hello", O_RDONLY, 0, f), 0);   // .. peste granita de mount
    file_put(f);
    EQ(vfs_umount(no_base, "/mnt"), -EBUSY);               // cwd in el
    EQ(vfs_chdir(cwd, "/mnt/.."), 0);
    EQ(vfs_getcwd(cwd, buf, sizeof(buf)), 2);
    vfs_path_release(cwd);

    f = open_ok("/mnt/f");
    EQ(vfs_umount(no_base, "/mnt"), -EBUSY);               // fisier deschis
    file_put(f);

    // Mount in mount: /mnt/c
    make_dir("/mnt/c");
    EQ(vfs_mount(no_base, "dev1", "/mnt/c", "memfs", 0, nullptr), 0);
    make_file("/mnt/c/g", "nested");
    EQ(vfs_umount(no_base, "/mnt"), -EBUSY);               // are un sub-mount
    CHECK(stat_ok("/mnt/c").st_dev != stat_ok("/mnt").st_dev);
    EQ(stat_ok("/mnt/c/..").st_dev, stat_ok("/mnt").st_dev);
    EQ(stat_ok("/mnt/c/../..").st_dev, root_dev);
    expect_content("/mnt/c/../c/g", "nested");
    EQ(vfs_umount(no_base, "/mnt/c"), 0);
    EQ(open_err("/mnt/c/g"), -ENOENT);
    EQ(vfs_umount(no_base, "/mnt"), 0);
    EQ(open_err("/mnt/f"), -ENOENT);
    EQ(stat_ok("/mnt").st_dev, root_dev);                  // din nou directorul gol din radacina
    EQ(vfs_umount(no_base, "/mnt"), -EINVAL);

    EQ(mount_count(), mounts);
    EQ(dentry_count(), dentries);
    EQ(inode_count(), inodes);
    make_file("/mnt/dupa", "x");                           // se poate scrie in directorul eliberat
    EQ(vfs_unlink(no_base, "/mnt/dupa"), 0);
}

struct Names {
    char name[16][24];
    uint8_t type[16];
    int count;
};

void parse_dirents(const void* buf, int64_t n, Names& out){
    for(int64_t off = 0; off < n;){
        const Dirent64* d = (const Dirent64*)((const char*)buf + off);
        CHECK(d->d_reclen % 8 == 0);
        CHECK(out.count < 16);
        memcpy(out.name[out.count], d->d_name, slen(d->d_name) + 1);
        out.type[out.count] = d->d_type;
        out.count++;
        off += d->d_reclen;
    }
}

bool has(const Names& n, const char* name, uint8_t type){
    for(int i = 0; i < n.count; i++){
        if(same(n.name[i], name)){
            return n.type[i] == type;
        }
    }
    return false;
}

void test_getdents(){
    make_dir("/ls");
    make_file("/ls/f1", "1");
    make_file("/ls/f2", "22");
    make_file("/ls/f3", "333");
    make_dir("/ls/d");
    EQ(vfs_symlink(no_base, "f1", "/ls/lnk"), 0);

    File* f = open_ok("/ls", O_RDONLY | O_DIRECTORY);
    uint64_t buf[256 / 8];
    Names n = {};
    int64_t r = f->ops->getdents(f, buf, sizeof(buf));
    CHECK(r > 0);
    parse_dirents(buf, r, n);
    EQ(f->ops->getdents(f, buf, sizeof(buf)), 0);          // sfarsit
    EQ(n.count, 7);
    CHECK(has(n, ".", DT_DIR) && has(n, "..", DT_DIR) && has(n, "f1", DT_REG) && has(n, "f2", DT_REG));
    CHECK(has(n, "f3", DT_REG) && has(n, "d", DT_DIR) && has(n, "lnk", DT_LNK));

    // Cu un buffer cat pentru o singura intrare, in mai multe apeluri
    EQ(f->ops->lseek(f, 0, SEEK_SET), 0);
    EQ(f->ops->getdents(f, buf, 8), -EINVAL);              // nu incape nici o intrare
    Names m = {};
    int calls = 0;
    for(;;){
        r = f->ops->getdents(f, buf, 32);
        if(r <= 0){
            EQ(r, 0);
            break;
        }
        parse_dirents(buf, r, m);
        calls++;
    }
    EQ(m.count, 7);
    CHECK(calls >= 4);
    EQ(f->ops->lseek(f, 1, SEEK_CUR), -EINVAL);            // director: doar rebobinare
    EQ(f->ops->read(f, buf, 8, false), -EISDIR);
    file_put(f);

    f = open_ok("/ls/f1");
    EQ(f->ops->getdents(f, buf, sizeof(buf)), -ENOTDIR);
    file_put(f);
    // Radacina unui mount nou are doar "." si ".."
    EQ(vfs_mount(no_base, "", "/ls/d", "memfs", 0, nullptr), 0);
    f = open_ok("/ls/d");
    Names e = {};
    r = f->ops->getdents(f, buf, sizeof(buf));
    parse_dirents(buf, r, e);
    EQ(e.count, 2);
    file_put(f);
    EQ(vfs_umount(no_base, "/ls/d"), 0);

    const char* names[] = {"/ls/f1", "/ls/f2", "/ls/f3", "/ls/lnk"};
    for(const char* nm : names){
        EQ(vfs_unlink(no_base, nm), 0);
    }
    EQ(vfs_rmdir(no_base, "/ls/d"), 0);
    EQ(vfs_rmdir(no_base, "/ls"), 0);
}

// Tabelul de descriptori din T4.6 peste File-urile VFS: dup prin fd_copy imparte pozitia, close pe rand
void test_fd_table(){
    FdTable t;
    memset(&t, 0, sizeof(t));
    make_file("/tmp/fdfile", "0123456789");
    File* f = open_ok("/tmp/fdfile", O_RDWR);
    int fd = fd_install(t, f);
    EQ(fd, 0);
    CHECK(fd_get(t, fd) == f);
    FdTable child;
    memset(&child, 0, sizeof(child));
    fd_copy(child, t);                                     // ca la fork
    EQ(f->refs, 2);

    char buf[16];
    EQ(fd_get(t, 0)->ops->read(fd_get(t, 0), buf, 4, false), 4);
    EQ(fd_get(child, 0)->ops->read(fd_get(child, 0), buf, 3, false), 3);
    CHECK(memcmp(buf, "456", 3) == 0);                    // pozitia comuna
    EQ(fd_close(t, 0), 0);
    EQ(f->refs, 1);
    EQ(fd_close(t, 0), -EBADF);
    uint64_t dentries = dentry_count();
    // Ultimul close elibereaza dentry-ul; fisierul sters cat timp e deschis dispare apoi
    EQ(vfs_unlink(no_base, "/tmp/fdfile"), 0);
    fd_close_all(child);
    EQ(dentry_count(), dentries - 1);

    // Descriptori multipli pe acelasi fisier au pozitii separate
    make_file("/tmp/two", "abcdef");
    FdTable u;
    memset(&u, 0, sizeof(u));
    EQ(fd_install(u, open_ok("/tmp/two")), 0);
    EQ(fd_install(u, open_ok("/tmp/two")), 1);
    EQ(u.fd[0]->ops->read(u.fd[0], buf, 2, false), 2);
    EQ(u.fd[1]->ops->read(u.fd[1], buf, 6, false), 6);
    CHECK(memcmp(buf, "abcdef", 6) == 0);
    EQ(u.fd[0]->ops->read(u.fd[0], buf, 6, false), 4);
    CHECK(memcmp(buf, "cdef", 4) == 0);
    fd_close_all(u);
    EQ(vfs_unlink(no_base, "/tmp/two"), 0);
}

// Creare / stergere repetata: nu raman dentry-uri, inode-uri sau mount-uri
void test_leaks(){
    uint64_t dentries = dentry_count();
    uint64_t inodes = inode_count();
    for(int round = 0; round < 200; round++){
        char name[16] = {'/', 't', 'm', 'p', '/', 'n', (char)('0' + round % 10), (char)('a' + round % 26), 0};
        make_file(name, "x");
        EQ(open_err(name), 0);
        EQ(vfs_unlink(no_base, name), 0);
        EQ(open_err(name), -ENOENT);
        EQ(stat_err("/tmp/nu-exista"), -ENOENT);           // dentry negativ: nu ramane in cache
    }
    EQ(dentry_count(), dentries);
    EQ(inode_count(), inodes);
    for(int round = 0; round < 20; round++){
        EQ(vfs_mount(no_base, "", "/mnt", "memfs", 0, nullptr), 0);
        make_dir("/mnt/x");
        make_file("/mnt/x/y", "y");
        EQ(vfs_umount(no_base, "/mnt"), 0);
    }
    EQ(dentry_count(), dentries);
    EQ(inode_count(), inodes);
}

// Syscall-urile din ring 3 (user/test/fstest.S) peste acelasi memfs montat la "/"
void test_user_syscalls(){
    static const char* const argv[] = {"fstest", nullptr};
    static const char* const envp[] = {"YOKAI=1", nullptr};
    uint64_t dentries = dentry_count();
    uint64_t inodes = inode_count();
    uint64_t tasks_before = task_count();
    uint64_t procs_before = process_count();
    uint64_t pid = exec_spawn_module("fstest", argv, envp);
    if(!pid){
        panic("test_vfs: fstest nu a pornit");
    }
    int code;
    uint64_t deadline = monotonic_ns() + 30000000000UL;
    while(!process_exit_status(pid, code)){
        if(monotonic_ns() > deadline){
            panic("test_vfs: fstest (pid %lu) nu a iesit in 30 s", pid);
        }
        yield();
        __asm__ volatile("hlt");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    if(code != 0){
        panic("test_vfs: fstest a iesit cu %d (verificarea %d din fstest.S a esuat)", code, code);
    }
    EQ(process_count(), procs_before);
    EQ(dentry_count(), dentries);                          // cwd-urile procesului si ale copilului s-au eliberat
    EQ(inode_count(), inodes);
}

}

void test_vfs_run(){
    test_files();
    kprintf("[OK] test_vfs files\n");
    test_paths();
    kprintf("[OK] test_vfs paths\n");
    test_symlinks();
    kprintf("[OK] test_vfs symlinks\n");
    test_mounts();
    kprintf("[OK] test_vfs mounts\n");
    test_getdents();
    kprintf("[OK] test_vfs getdents\n");
    test_fd_table();
    kprintf("[OK] test_vfs fd\n");
    test_leaks();
    __asm__ volatile("sti");
    test_user_syscalls();
    __asm__ volatile("cli");
    kprintf("[OK] test_vfs syscalls\n");
    kprintf("[OK] test_vfs (memfs: open/read/write/lseek/stat, cai relative, symlink max %u, mount/umount, getdents, FdTable, syscall-uri din ring 3, fara scurgeri)\n",
            (unsigned)MAX_SYMLINKS);
}
