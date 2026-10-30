#include "tests/test_tarfs.h"
#include "uapi/syscall_nr.h"
#include "fs/vfs/vfs.h"
#include "fs/tarfs.h"
#include "exec/elf.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_tarfs:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_tarfs:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};

const char* const LAYOUT[] = {"bin", "boot", "dev", "etc", "home", "lib", "mnt", "media", "opt", "proc", "root",
                              "run", "sbin", "sys", "tmp", "usr", "var"};

Stat stat_ok(const char* path, bool follow = true){
    Stat st;
    EQ(vfs_stat(none, path, follow, st), 0);
    return st;
}

int open_err(const char* path, int flags){
    File* f;
    int err = vfs_open(none, path, flags, 0644, f);
    if(!err){
        file_put(f);
    }
    return err;
}

uint64_t slen(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

bool same(const char* a, const char* b){
    uint64_t n = slen(a);
    return n == slen(b) && memcmp(a, b, n) == 0;
}

// Citeste tot fisierul in buf (cu terminator); numarul de bytes
int64_t slurp(const char* path, char* buf, uint64_t cap){
    File* f;
    EQ(vfs_open(none, path, O_RDONLY, 0, f), 0);
    int64_t total = 0;
    for(;;){
        int64_t n = f->ops->read(f, buf + total, cap - 1 - (uint64_t)total, false);
        EQ(n >= 0, 1);
        if(n == 0){
            break;
        }
        total += n;
    }
    buf[total] = 0;
    file_put(f);
    return total;
}

void check_layout(){
    CHECK(S_ISDIR(stat_ok("/").st_mode));
    for(const char* d : LAYOUT){
        char p[32] = "/";
        memcpy(p + 1, d, slen(d) + 1);
        CHECK(S_ISDIR(stat_ok(p).st_mode));
    }
    // getdents pe "/": toate directoarele din layout apar o data
    File* f;
    EQ(vfs_open(none, "/", O_RDONLY | O_DIRECTORY, 0, f), 0);
    static char buf[2048];
    int seen[sizeof(LAYOUT) / sizeof(LAYOUT[0])] = {};
    int64_t got;
    while((got = f->ops->getdents(f, buf, sizeof(buf))) > 0){
        for(int64_t off = 0; off < got;){
            const Dirent64* d = (const Dirent64*)(buf + off);
            off += d->d_reclen;
            for(unsigned i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]); i++){
                if(same(d->d_name, LAYOUT[i])){
                    seen[i]++;
                    EQ(d->d_type, DT_DIR);
                }
            }
        }
    }
    EQ(got, 0);
    file_put(f);
    for(unsigned i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]); i++){
        EQ(seen[i], 1);
    }
}

void check_files(){
    char buf[256];
    Stat st = stat_ok("/etc/hostname");
    CHECK(S_ISREG(st.st_mode));
    EQ(st.st_mode & 07777, 0644);
    EQ(st.st_size, 12);
    EQ(slurp("/etc/hostname", buf, sizeof(buf)), 12);
    CHECK(same(buf, "yokai-drone\n"));

    // lseek + read partial
    File* f;
    EQ(vfs_open(none, "/etc/hostname", O_RDONLY, 0, f), 0);
    EQ(f->ops->lseek(f, 6, SEEK_SET), 6);
    EQ(f->ops->read(f, buf, 3, false), 3);
    CHECK(memcmp(buf, "dro", 3) == 0);
    EQ(f->ops->lseek(f, 0, SEEK_END), 12);
    EQ(f->ops->read(f, buf, 8, false), 0);
    file_put(f);

    // Programele user din initrd sunt ELF
    EQ(vfs_open(none, "/bin/hello", O_RDONLY, 0, f), 0);
    EQ(f->ops->read(f, buf, 4, false), 4);
    CHECK(buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F');
    file_put(f);
    CHECK((stat_ok("/bin/hello").st_mode & 0111) == 0111);
}

void check_paths_and_links(){
    char buf[64];
    CHECK(S_ISDIR(stat_ok("/etc/../bin/./").st_mode));
    CHECK(S_ISREG(stat_ok("/bin/../etc//hostname").st_mode));
    // /etc/mtab -> ../proc/mounts (tinta nu exista: /proc e gol)
    CHECK(S_ISLNK(stat_ok("/etc/mtab", false).st_mode));
    EQ(vfs_readlink(none, "/etc/mtab", buf, sizeof(buf)), 14);
    CHECK(memcmp(buf, "../proc/mounts", 14) == 0);
    Stat st;
    EQ(vfs_stat(none, "/etc/mtab", true, st), -ENOENT);
    EQ(open_err("/nu-exista", O_RDONLY), -ENOENT);
    EQ(open_err("/etc/hostname/x", O_RDONLY), -ENOTDIR);
    EQ(open_err("/etc/hostname", O_RDONLY | O_DIRECTORY), -ENOTDIR);
    EQ(open_err("/etc", O_RDONLY), 0);
}

void check_read_only(){
    EQ(open_err("/etc/hostname", O_WRONLY), -EROFS);
    EQ(open_err("/etc/hostname", O_RDWR), -EROFS);
    EQ(open_err("/etc/hostname", O_RDONLY | O_TRUNC), -EROFS);
    EQ(open_err("/etc/nou", O_CREAT | O_WRONLY), -EROFS);
    EQ(vfs_mkdir(none, "/etc/d", 0755), -EROFS);
    EQ(vfs_symlink(none, "x", "/etc/l"), -EROFS);
    EQ(vfs_unlink(none, "/etc/hostname"), -EROFS);
    EQ(vfs_rmdir(none, "/home"), -EROFS);
    char buf[16];
    EQ(slurp("/etc/hostname", buf, sizeof(buf)), 12);          // continutul a ramas neatins
}

void check_mounts(){
    uint64_t mounts = mount_count();
    uint64_t inodes = inode_count();
    EQ(vfs_mount(none, "nu-exista", "/mnt", "tarfs", MS_RDONLY, nullptr), -ENOENT);
    EQ(mount_count(), mounts);
    EQ(inode_count(), inodes);                                  // montarea esuata nu lasa inode-uri
    // Aceeasi arhiva inca o data, pe /mnt
    EQ(vfs_mount(none, "initrd", "/mnt", "tarfs", MS_RDONLY, nullptr), 0);
    CHECK(inode_count() > inodes);
    CHECK(stat_ok("/mnt/etc/hostname").st_dev != stat_ok("/etc/hostname").st_dev);
    EQ(stat_ok("/mnt/etc/hostname").st_size, 12);
    EQ(stat_ok("/mnt/..").st_dev, stat_ok("/").st_dev);
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(mount_count(), mounts);
    EQ(inode_count(), inodes);                                  // umount elibereaza tot arborele
    EQ(open_err("/mnt/etc/hostname", O_RDONLY), -ENOENT);
}

void check_exec(){
    static const char* const argv[] = {"x", nullptr};
    static const char* const envp[] = {nullptr};
    EQ(exec_spawn("/nu-exista", argv, envp), 0);
    EQ(exec_spawn("/etc", argv, envp), 0);                     // director
    EQ(exec_spawn("/etc/hostname", argv, envp), 0);            // nu e ELF
}

}

void test_tarfs_run(){
    check_layout();
    kprintf("[OK] test_tarfs layout\n");
    check_files();
    kprintf("[OK] test_tarfs files\n");
    check_paths_and_links();
    kprintf("[OK] test_tarfs paths\n");
    check_read_only();
    kprintf("[OK] test_tarfs read-only\n");
    check_mounts();
    kprintf("[OK] test_tarfs mounts\n");
    check_exec();
    kprintf("[OK] test_tarfs (initrd pe /: layout, citire, symlink, read-only, montari multiple, exec invalid)\n");
}
