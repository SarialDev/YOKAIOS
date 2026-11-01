#include "tests/test_iso9660.h"
#include "fs/iso9660.h"
#include "fs/vfs/vfs.h"
#include "block/bcache.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_iso9660:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_iso9660:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};
const uint64_t BIG_SIZE = 300000;

uint8_t buf[8192];
uint8_t buf2[8192];
char names[8192];

uint64_t strlen_(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

uint8_t pat(uint64_t i){
    return (uint8_t)(i * 131 + (i >> 9));
}

File* open_ok(const char* path, int flags){
    File* f;
    EQ(vfs_open(none, path, flags, 0644, f), 0);
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

int64_t rd(File* f, void* b, uint64_t n){
    return f->ops->read(f, b, n, false);
}

bool text_is(const char* path, const char* text){
    File* f = open_ok(path, O_RDONLY);
    uint64_t n = strlen_(text);
    int64_t got = rd(f, buf2, sizeof(buf2));
    file_put(f);
    return got == (int64_t)n && memcmp(buf2, text, n) == 0;
}

uint64_t size_of(const char* path){
    Stat st;
    EQ(vfs_stat(none, path, true, st), 0);
    return (uint64_t)st.st_size;
}

// ",nume1,nume2," cu numele din director (fara . si ..)
int list_dir(const char* path){
    File* f = open_ok(path, O_RDONLY | O_DIRECTORY);
    uint64_t n = 0;
    int count = 0;
    names[n++] = ',';
    int64_t got;
    while((got = f->ops->getdents(f, buf, sizeof(buf))) > 0){
        for(int64_t off = 0; off < got;){
            Dirent64* d = (Dirent64*)(buf + off);
            off += d->d_reclen;
            if(d->d_name[0] == '.' && (d->d_name[1] == 0 || (d->d_name[1] == '.' && d->d_name[2] == 0))){
                continue;
            }
            for(const char* c = d->d_name; *c && n + 3 < sizeof(names); c++){
                names[n++] = *c;
            }
            names[n++] = ',';
            count++;
        }
    }
    EQ(got, 0);
    names[n] = 0;
    file_put(f);
    return count;
}

bool listed(const char* name){
    uint64_t nl = strlen_(name);
    for(const char* p = names; *p; p++){
        if(*p != ',' || p[1] == 0){
            continue;
        }
        uint64_t k = 0;
        while(k < nl && p[1 + k] == name[k]){
            k++;
        }
        if(k == nl && p[1 + k] == ','){
            return true;
        }
    }
    return false;
}

void check_boot_cd(){
    // Montat de main.cpp pe /media/cdrom din sr0
    int n = list_dir("/media/cdrom");
    CHECK(n >= 1);
    kprintf("ISO ls /media/cdrom: %s\n", names);
    CHECK(listed("boot"));
    EQ(list_dir("/media/cdrom/boot") >= 3, 1);
    CHECK(listed("kernel.bin") && listed("initrd.tar") && listed("grub"));
    File* f = open_ok("/media/cdrom/boot/kernel.bin", O_RDONLY);
    EQ(rd(f, buf2, 8), 8);
    CHECK(memcmp(buf2, "\x7f" "ELF", 4) == 0);
    file_put(f);
    CHECK(size_of("/media/cdrom/boot/kernel.bin") > 100000);
    f = open_ok("/media/cdrom/boot/initrd.tar", O_RDONLY);
    EQ(rd(f, buf2, 512), 512);
    CHECK(memcmp(buf2 + 257, "ustar", 5) == 0);                 // antet tar
    file_put(f);
    f = open_ok("/media/cdrom/boot/grub/grub.cfg", O_RDONLY);
    int64_t got = rd(f, buf2, sizeof(buf2) - 1);
    file_put(f);
    CHECK(got > 20);
    buf2[got] = 0;
    bool found = false;
    for(int64_t i = 0; i + 10 < got; i++){
        if(memcmp(buf2 + i, "multiboot2", 10) == 0){
            found = true;
        }
    }
    CHECK(found);
    EQ(open_err("/media/cdrom/boot/grub/grub.cfg", O_WRONLY), -EROFS);
    EQ(open_err("/media/cdrom/nou.txt", O_CREAT | O_WRONLY), -EROFS);
    EQ(vfs_mkdir(none, "/media/cdrom/dir", 0755), -EROFS);
    EQ(vfs_unlink(none, "/media/cdrom/boot/kernel.bin"), -EROFS);
    EQ(open_err("/media/cdrom/nu_exista", O_RDONLY), -ENOENT);
    kprintf("[OK] test_iso9660 cd de boot\n");
}

void check_tree(const char* base){
    char p[256];
    auto path = [&](const char* rel){
        uint64_t n = strlen_(base);
        memcpy(p, base, n);
        uint64_t m = strlen_(rel);
        memcpy(p + n, rel, m + 1);
        return (const char*)p;
    };

    // Radacina: 20 de intrari, ordinea nu conteaza
    int n = list_dir(base);
    kprintf("ISO ls %s: %d intrari\n", base, n);
    CHECK(listed("hello.txt") && listed("Long Mixed Case Name.TXT") && listed("empty") && listed("emptydir"));
    CHECK(listed("run.sh") && listed("big.bin") && listed("sub") && listed("deep") && listed("many"));
    CHECK(listed("link") && listed("abslink") && listed("deeplink"));
    CHECK(listed("\xC8\x9A" "ar\xC4\x83.txt"));
    CHECK(!listed("HELLO.TXT;1") && !listed("."));

    CHECK(text_is(path("/hello.txt"), "hello from iso\n"));
    CHECK(text_is(path("/Long Mixed Case Name.TXT"), "long name\n"));
    CHECK(text_is(path("/\xC8\x9A" "ar\xC4\x83.txt"), "unicode \xC3\xBC" "ber\n"));
    CHECK(text_is(path("/sub/deeper/nested.txt"), "nested file\n"));
    EQ(size_of(path("/empty")), 0);
    EQ(open_err(path("/emptydir"), O_RDONLY | O_DIRECTORY), 0);
    EQ(list_dir(path("/emptydir")), 0);
    EQ(open_err(path("/hello.txt/x"), O_RDONLY), -ENOTDIR);
    EQ(open_err(path("/nu_exista.txt"), O_RDONLY), -ENOENT);
    EQ(open_err(path("/hello.txt"), O_WRONLY), -EROFS);

    // Nume de 200 de caractere (zona System Use continuata cu CE)
    char longname[256];
    uint64_t bl = strlen_(base);
    memcpy(longname, base, bl);
    longname[bl] = '/';
    for(int i = 0; i < 200; i++){
        longname[bl + 1 + i] = 'y';
    }
    memcpy(longname + bl + 201, ".txt", 5);
    CHECK(text_is(longname, "long200\n"));

    // Moduri, timp
    Stat st;
    EQ(vfs_stat(none, path("/run.sh"), true, st), 0);
    CHECK(S_ISREG(st.st_mode) && (st.st_mode & 0111) == 0111);
    EQ(vfs_stat(none, path("/hello.txt"), true, st), 0);
    CHECK(S_ISREG(st.st_mode) && (st.st_mode & 0200) == 0 && (st.st_mode & 0444) == 0444);
    CHECK(st.st_mtim.sec > 1577836800);                          // dupa 2020-01-01
    EQ(st.st_size, 15);
    EQ(vfs_stat(none, path("/sub"), true, st), 0);
    CHECK(S_ISDIR(st.st_mode));

    // Symlink-uri
    char lb[128];
    EQ(vfs_readlink(none, path("/link"), lb, sizeof(lb)), 9);
    CHECK(memcmp(lb, "hello.txt", 9) == 0);
    EQ(vfs_readlink(none, path("/abslink"), lb, sizeof(lb)), 11);
    CHECK(memcmp(lb, "/abs/target", 11) == 0);
    EQ(vfs_readlink(none, path("/sub/uplink"), lb, sizeof(lb)), 12);
    CHECK(memcmp(lb, "../hello.txt", 12) == 0);
    CHECK(text_is(path("/link"), "hello from iso\n"));
    CHECK(text_is(path("/sub/uplink"), "hello from iso\n"));
    CHECK(text_is(path("/deeplink/nested.txt"), "nested file\n"));
    EQ(vfs_stat(none, path("/link"), false, st), 0);
    CHECK(S_ISLNK(st.st_mode));
    EQ(st.st_size, 9);
    EQ(open_err(path("/abslink"), O_RDONLY), -ENOENT);           // tinta nu exista pe /

    // Director cu 300 de intrari (mai multe sectoare de 2 KiB)
    EQ(list_dir(path("/many")), 300);
    CHECK(listed("f0000.txt") && listed("f0150.txt") && listed("f0299.txt") && !listed("f0300.txt"));
    CHECK(text_is(path("/many/f0000.txt"), "file 0000\n"));
    CHECK(text_is(path("/many/f0299.txt"), "file 0299\n"));
    CHECK(text_is(path("/many/f0137.txt"), "file 0137\n"));

    // Director adanc (12 niveluri): relocat cu CL, continutul trebuie sa fie accesibil prin calea normala
    CHECK(text_is(path("/deep/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12/leaf.txt"), "leaf\n"));
    EQ(list_dir(path("/deep/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/d12")), 1);
    CHECK(listed("leaf.txt"));
    CHECK(text_is(path("/deep/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/../d11/d12/leaf.txt"), "leaf\n"));

    // Fisier mare, citiri la margini de sectoare
    EQ(size_of(path("/big.bin")), BIG_SIZE);
    File* f = open_ok(path("/big.bin"), O_RDONLY);
    uint64_t pos = 0;
    int64_t got;
    while((got = rd(f, buf2, 4093)) > 0){
        for(int64_t i = 0; i < got; i++){
            if(buf2[i] != pat(pos + i)){
                panic("test_iso9660: big.bin octet gresit la %lu (%x, asteptat %x)", pos + i, buf2[i], pat(pos + i));
            }
        }
        pos += got;
    }
    EQ(got, 0);
    EQ(pos, BIG_SIZE);
    static const uint64_t offs[] = {1, 2047, 2048, 2049, 4095, 4096, 4097, 8191, 65535, 299990};
    for(uint64_t o : offs){
        EQ(f->ops->lseek(f, (int64_t)o, SEEK_SET), (int64_t)o);
        uint64_t want = BIG_SIZE - o < 100 ? BIG_SIZE - o : 100;
        EQ(rd(f, buf2, 100), (int64_t)want);
        for(uint64_t i = 0; i < want; i++){
            CHECK(buf2[i] == pat(o + i));
        }
    }
    file_put(f);
    kprintf("[OK] test_iso9660 rock ridge\n");
}

}

void test_iso9660_run(){
    check_boot_cd();
    EQ(vfs_mount(none, "sr1", "/mnt", "iso9660", MS_RDONLY, nullptr), 0);
    check_tree("/mnt");
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(open_err("/mnt/hello.txt", O_RDONLY), -ENOENT);

    // Fara Rock Ridge: numele ISO (litere mici, fara ;1); cele lungi se trunchiaza
    EQ(vfs_mount(none, "sr1", "/mnt", "iso9660", MS_RDONLY, "norr"), 0);
    list_dir("/mnt");
    CHECK(listed("hello.txt") && listed("many") && listed("big.bin"));
    CHECK(!listed("Long Mixed Case Name.TXT"));
    Stat st;
    EQ(vfs_stat(none, "/mnt/link", false, st), 0);
    CHECK(S_ISREG(st.st_mode));                                  // fara Rock Ridge symlink-ul e un fisier gol
    CHECK(text_is("/mnt/HELLO.TXT", "hello from iso\n"));        // fara diferenta de majuscule
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(vfs_mount(none, "sr1", "/mnt", "iso9660", MS_RDONLY, "xyz"), -EINVAL);
    EQ(vfs_mount(none, "sr9", "/mnt", "iso9660", MS_RDONLY, nullptr), -ENODEV);
    kprintf("[OK] test_iso9660 fara rock ridge\n");

    // CD-ul de boot se poate monta si a doua oara
    EQ(vfs_mount(none, "sr0", "/mnt", "iso9660", MS_RDONLY, nullptr), 0);
    EQ(vfs_umount(none, "/mnt"), 0);
    kprintf("[OK] test_iso9660 (ISO 9660: PVD, Rock Ridge NM/PX/SL/TF/CE/CL, CD de boot pe AHCI/ATAPI, bcache)\n");
}
