#include "tests/test_tmpfs.h"
#include "uapi/syscall_nr.h"
#include "fs/vfs/vfs.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_tmpfs:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_tmpfs:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};
const uint64_t PG = 4096;

uint8_t big[2 * PG];

Stat stat_ok(const char* path){
    Stat st;
    EQ(vfs_stat(none, path, true, st), 0);
    return st;
}

File* open_ok(const char* path, int flags, uint32_t mode = 0644){
    File* f;
    EQ(vfs_open(none, path, flags, mode, f), 0);
    return f;
}

int open_err(const char* path, int flags){
    File* f;
    int err = vfs_open(none, path, flags, 0644, f);
    if(!err){
        file_put(f);
    }
    return err;
}

int64_t wr(File* f, const void* buf, uint64_t n){
    return f->ops->write(f, buf, n, false);
}

int64_t rd(File* f, void* buf, uint64_t n){
    return f->ops->read(f, buf, n, false);
}

void seek(File* f, uint64_t pos){
    EQ(f->ops->lseek(f, (int64_t)pos, SEEK_SET), (int64_t)pos);
}

uint8_t pat(uint64_t off){
    return (uint8_t)(off * 131 + (off >> 12));
}

// Numele din director, separate prin virgula (fara . si ..)
void list_dir(const char* path, char* out, uint64_t cap){
    File* f = open_ok(path, O_RDONLY | O_DIRECTORY);
    uint64_t n = 0;
    out[0] = 0;
    int64_t got;
    while((got = f->ops->getdents(f, big, sizeof(big))) > 0){
        for(int64_t off = 0; off < got;){
            Dirent64* d = (Dirent64*)(big + off);
            off += d->d_reclen;
            if(d->d_name[0] == '.' && (d->d_name[1] == 0 || (d->d_name[1] == '.' && d->d_name[2] == 0))){
                continue;
            }
            for(const char* c = d->d_name; *c && n + 2 < cap; c++){
                out[n++] = *c;
            }
            if(n + 2 < cap){
                out[n++] = ',';
            }
        }
    }
    EQ(got, 0);
    out[n] = 0;
    file_put(f);
}

bool same(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

void check_mounts(){
    Stat tmp = stat_ok("/tmp");
    Stat run = stat_ok("/run");
    Stat root = stat_ok("/");
    CHECK(S_ISDIR(tmp.st_mode) && (tmp.st_mode & 07777) == 01777);
    CHECK(S_ISDIR(run.st_mode) && (run.st_mode & 07777) == 0755);
    CHECK(tmp.st_dev != root.st_dev && run.st_dev != root.st_dev && tmp.st_dev != run.st_dev);
    EQ(vfs_rmdir(none, "/tmp"), -EBUSY);
    // Fisier in /run, separat de /tmp
    File* f = open_ok("/run/rx", O_CREAT | O_RDWR);
    EQ(wr(f, "run", 3), 3);
    file_put(f);
    EQ(open_err("/tmp/rx", O_RDONLY), -ENOENT);
    EQ(vfs_unlink(none, "/run/rx"), 0);
}

void check_files(){
    char buf[32];
    File* f = open_ok("/tmp/hello", O_CREAT | O_RDWR);
    EQ(wr(f, "hello world", 11), 11);
    seek(f, 0);
    EQ(rd(f, buf, sizeof(buf)), 11);
    CHECK(memcmp(buf, "hello world", 11) == 0);
    EQ(rd(f, buf, sizeof(buf)), 0);
    Stat st = stat_ok("/tmp/hello");
    EQ(st.st_size, 11);
    EQ(st.st_mode, S_IFREG | 0644);
    EQ(st.st_nlink, 1);
    file_put(f);

    f = open_ok("/tmp/hello", O_WRONLY | O_APPEND);
    EQ(wr(f, "!!", 2), 2);
    file_put(f);
    f = open_ok("/tmp/hello", O_RDONLY);
    EQ(rd(f, buf, sizeof(buf)), 13);
    CHECK(memcmp(buf, "hello world!!", 13) == 0);
    EQ(wr(f, "x", 1), -EBADF);
    file_put(f);

    EQ(open_err("/tmp/hello", O_CREAT | O_EXCL | O_WRONLY), -EEXIST);
    f = open_ok("/tmp/hello", O_WRONLY | O_TRUNC);
    file_put(f);
    EQ(stat_ok("/tmp/hello").st_size, 0);
    EQ(vfs_unlink(none, "/tmp/hello"), 0);
    EQ(open_err("/tmp/hello", O_RDONLY), -ENOENT);
}

void check_pages(){
    // Gaura intre pagini: se citeste ca zerouri si nu ocupa memorie
    File* f = open_ok("/tmp/sparse", O_CREAT | O_RDWR);
    seek(f, 3 * PG + 10);
    EQ(wr(f, "abc", 3), 3);
    EQ(stat_ok("/tmp/sparse").st_size, 3 * PG + 13);
    seek(f, 0);
    EQ(rd(f, big, 2 * PG), 2 * PG);
    for(uint64_t i = 0; i < 2 * PG; i++){
        CHECK(big[i] == 0);
    }
    seek(f, 3 * PG + 8);
    EQ(rd(f, big, 16), 5);
    CHECK(big[0] == 0 && big[1] == 0 && memcmp(big + 2, "abc", 3) == 0);
    EQ(rd(f, big, 16), 0);
    file_put(f);
    EQ(vfs_unlink(none, "/tmp/sparse"), 0);

    // 1 MiB cu model, scris cu pasi de 777 si citit cu pasi de 1000 (nealiniati la pagina), apoi sters: nu se pierd pagini
    uint64_t before = pmm_free_count();
    const uint64_t size = 256 * PG;
    f = open_ok("/tmp/big", O_CREAT | O_RDWR);
    for(uint64_t off = 0; off < size;){
        uint64_t n = size - off < 777 ? size - off : 777;
        for(uint64_t i = 0; i < n; i++){
            big[i] = pat(off + i);
        }
        EQ(wr(f, big, n), (int64_t)n);
        off += n;
    }
    EQ(stat_ok("/tmp/big").st_size, (int64_t)size);
    seek(f, 0);
    for(uint64_t off = 0; off < size;){
        uint64_t n = size - off < 1000 ? size - off : 1000;
        EQ(rd(f, big, n), (int64_t)n);
        for(uint64_t i = 0; i < n; i++){
            if(big[i] != pat(off + i)){
                panic("test_tmpfs: continut gresit la offset %lu", off + i);
            }
        }
        off += n;
    }
    CHECK(pmm_free_count() + 256 <= before);                    // paginile chiar s-au alocat
    file_put(f);
    EQ(vfs_unlink(none, "/tmp/big"), 0);
    CHECK(pmm_free_count() + 8 >= before);
}

void check_truncate(){
    File* f = open_ok("/tmp/t", O_CREAT | O_RDWR);
    memset(big, 0x55, 6000);
    EQ(wr(f, big, 6000), 6000);
    Inode* ino = f->inode;
    EQ(ino->ops->truncate(ino, 100), 0);
    EQ(stat_ok("/tmp/t").st_size, 100);
    EQ(ino->ops->truncate(ino, 6000), 0);                       // extinderea expune zerouri, nu datele vechi
    seek(f, 0);
    EQ(rd(f, big, 6000), 6000);
    for(uint64_t i = 0; i < 6000; i++){
        CHECK(big[i] == (i < 100 ? 0x55 : 0));
    }
    // Scriere dupa sfarsit in aceeasi pagina: golul ramane zero
    EQ(ino->ops->truncate(ino, 10), 0);
    seek(f, 20);
    EQ(wr(f, "z", 1), 1);
    seek(f, 0);
    EQ(rd(f, big, 64), 21);
    CHECK(big[9] == 0x55 && big[10] == 0 && big[19] == 0 && big[20] == 'z');
    EQ(ino->ops->truncate(ino, (1UL << 30) + 1), -EFBIG);
    file_put(f);
    f = open_ok("/tmp/t", O_RDWR | O_TRUNC);
    EQ(f->inode->size, 0);
    file_put(f);
    EQ(vfs_unlink(none, "/tmp/t"), 0);
}

void check_unlinked_open(){
    uint64_t inodes = inode_count();
    uint64_t before = pmm_free_count();
    File* f = open_ok("/tmp/gone", O_CREAT | O_RDWR);
    EQ(wr(f, "data", 4), 4);
    EQ(vfs_unlink(none, "/tmp/gone"), 0);
    EQ(open_err("/tmp/gone", O_RDONLY), -ENOENT);
    EQ(stat_ok("/tmp").st_nlink, 2);
    seek(f, 0);
    char buf[8];
    EQ(rd(f, buf, 8), 4);
    CHECK(memcmp(buf, "data", 4) == 0);
    file_put(f);
    EQ(inode_count(), inodes);
    CHECK(pmm_free_count() + 4 >= before);
}

void check_dirs(){
    char list[64];
    EQ(vfs_mkdir(none, "/tmp/d", 0750), 0);
    EQ(stat_ok("/tmp").st_nlink, 3);
    EQ(stat_ok("/tmp/d").st_mode, S_IFDIR | 0750);
    File* f = open_ok("/tmp/d/a", O_CREAT | O_WRONLY);
    EQ(wr(f, "A", 1), 1);
    file_put(f);
    file_put(open_ok("/tmp/d/b", O_CREAT | O_WRONLY));
    EQ(vfs_symlink(none, "a", "/tmp/d/l"), 0);
    EQ(vfs_mkdir(none, "/tmp/d/sub", 0755), 0);
    EQ(vfs_mkdir(none, "/tmp/d", 0755), -EEXIST);
    list_dir("/tmp/d", list, sizeof(list));
    CHECK(same(list, "a,b,l,sub,"));                            // ordinea crearii
    EQ(vfs_rmdir(none, "/tmp/d"), -ENOTEMPTY);

    char buf[8];
    EQ(vfs_readlink(none, "/tmp/d/l", buf, sizeof(buf)), 1);
    CHECK(buf[0] == 'a');
    f = open_ok("/tmp/d/l", O_RDONLY);
    EQ(rd(f, buf, sizeof(buf)), 1);
    CHECK(buf[0] == 'A');
    file_put(f);
    EQ(open_err("/tmp/d/a", O_RDONLY | O_DIRECTORY), -ENOTDIR);
    EQ(vfs_unlink(none, "/tmp/d/sub"), -EISDIR);

    EQ(vfs_unlink(none, "/tmp/d/a"), 0);
    EQ(vfs_unlink(none, "/tmp/d/l"), 0);
    list_dir("/tmp/d", list, sizeof(list));
    CHECK(same(list, "b,sub,"));
    EQ(vfs_rmdir(none, "/tmp/d/sub"), 0);
    EQ(vfs_unlink(none, "/tmp/d/b"), 0);
    EQ(vfs_rmdir(none, "/tmp/d"), 0);
    EQ(stat_ok("/tmp").st_nlink, 2);
}

void check_limit(){
    uint64_t inodes = inode_count();
    uint64_t before = pmm_free_count();
    EQ(vfs_mount(none, "tmpfs", "/mnt", "tmpfs", 0, "size=abc"), -EINVAL);
    EQ(vfs_mount(none, "tmpfs", "/mnt", "tmpfs", 0, "bogus=1"), -EINVAL);
    EQ(vfs_mount(none, "tmpfs", "/mnt", "tmpfs", 0, "mode=9"), -EINVAL);
    EQ(inode_count(), inodes);
    EQ(vfs_mount(none, "tmpfs", "/mnt", "tmpfs", 0, "size=16,mode=0700"), 0);
    EQ(stat_ok("/mnt").st_mode, S_IFDIR | 0700);
    // 16 KiB = 4 pagini: tabela de nivel 1, o frunza si doar 2 pagini de date
    File* f = open_ok("/mnt/f", O_CREAT | O_RDWR);
    static uint8_t chunk[16 * PG];
    int64_t n = wr(f, chunk, sizeof(chunk));
    EQ(n, 2 * PG);
    EQ(wr(f, chunk, 1), -ENOSPC);
    EQ(stat_ok("/mnt/f").st_size, 2 * PG);
    EQ(vfs_unlink(none, "/mnt/f"), 0);                          // pagina inca deschisa: se elibereaza la close
    file_put(f);
    f = open_ok("/mnt/g", O_CREAT | O_WRONLY);                  // bugetul s-a eliberat
    EQ(wr(f, chunk, PG), PG);
    file_put(f);
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(inode_count(), inodes);
    CHECK(pmm_free_count() + 8 >= before);
}

}

void test_tmpfs_run(){
    check_mounts();
    kprintf("[OK] test_tmpfs mounts\n");
    check_files();
    kprintf("[OK] test_tmpfs files\n");
    check_pages();
    kprintf("[OK] test_tmpfs pages\n");
    check_truncate();
    kprintf("[OK] test_tmpfs truncate\n");
    check_unlinked_open();
    kprintf("[OK] test_tmpfs unlink-open\n");
    check_dirs();
    kprintf("[OK] test_tmpfs dirs\n");
    check_limit();
    kprintf("[OK] test_tmpfs limit\n");
    kprintf("[OK] test_tmpfs (pagini la cerere, goluri, truncate, directoare, symlink, limita size=, fara scurgeri)\n");
}
