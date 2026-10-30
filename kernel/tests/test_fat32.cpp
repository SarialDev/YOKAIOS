#include "tests/test_fat32.h"
#include "fs/fat32/fat32.h"
#include "fs/vfs/vfs.h"
#include "block/bcache.h"
#include "core/panic.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_fat32:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_fat32:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};
const uint64_t BIG_SIZE = 300000;                   // big.bin din imaginea gazdei

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

uint8_t pat(uint64_t off, uint8_t seed){
    return (uint8_t)(off * 131 + (off >> 9) + seed);
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

int64_t wr(File* f, const void* b, uint64_t n){
    return f->ops->write(f, b, n, false);
}

int64_t rd(File* f, void* b, uint64_t n){
    return f->ops->read(f, b, n, false);
}

void seek(File* f, uint64_t pos){
    EQ(f->ops->lseek(f, (int64_t)pos, SEEK_SET), (int64_t)pos);
}

uint64_t size_of(const char* path){
    Stat st;
    EQ(vfs_stat(none, path, true, st), 0);
    return (uint64_t)st.st_size;
}

bool exists(const char* path){
    Stat st;
    return vfs_stat(none, path, true, st) == 0;
}

// Scrie len octeti cu modelul (seed) de la pozitia curenta, in bucati de dimensiuni neobisnuite
void write_pat(File* f, uint64_t from, uint64_t len, uint8_t seed, uint32_t chunk = 4093){
    uint64_t done = 0;
    while(done < len){
        uint64_t n = len - done < chunk ? len - done : chunk;
        for(uint64_t i = 0; i < n; i++){
            buf[i] = pat(from + done + i, seed);
        }
        EQ(wr(f, buf, n), (int64_t)n);
        done += n;
    }
}

// Citeste len octeti de la pozitia curenta si verifica modelul pe [from, from + len)
void check_part(File* f, uint64_t from, uint64_t len, uint8_t seed, uint32_t chunk = 4093){
    uint64_t done = 0;
    while(done < len){
        uint64_t n = len - done < chunk ? len - done : chunk;
        EQ(rd(f, buf2, n), (int64_t)n);
        for(uint64_t i = 0; i < n; i++){
            if(buf2[i] != pat(from + done + i, seed)){
                panic("test_fat32: octet gresit la %lu (%x, asteptat %x)", from + done + i, buf2[i],
                      pat(from + done + i, seed));
            }
        }
        done += n;
    }
}

// Ca check_part, apoi sfarsit de fisier
void check_pat(File* f, uint64_t from, uint64_t len, uint8_t seed, uint32_t chunk = 4093){
    check_part(f, from, len, seed, chunk);
    EQ(rd(f, buf2, 16), 0);
}

// len octeti de la pozitia curenta trebuie sa fie zero
void check_zero(File* f, uint64_t len){
    while(len){
        uint64_t n = len < sizeof(buf2) ? len : sizeof(buf2);
        EQ(rd(f, buf2, n), (int64_t)n);
        for(uint64_t i = 0; i < n; i++){
            CHECK(buf2[i] == 0);
        }
        len -= n;
    }
}

void put_file(const char* path, uint64_t len, uint8_t seed){
    File* f = open_ok(path, O_CREAT | O_EXCL | O_WRONLY);
    write_pat(f, 0, len, seed);
    file_put(f);
}

void verify_file(const char* path, uint64_t len, uint8_t seed){
    EQ(size_of(path), len);
    File* f = open_ok(path, O_RDONLY);
    check_pat(f, 0, len, seed);
    file_put(f);
}

void put_text(const char* path, const char* text){
    File* f = open_ok(path, O_CREAT | O_WRONLY | O_TRUNC);
    uint64_t n = strlen_(text);
    EQ(wr(f, text, n), (int64_t)n);
    file_put(f);
}

bool text_is(const char* path, const char* text){
    File* f = open_ok(path, O_RDONLY);
    uint64_t n = strlen_(text);
    int64_t got = rd(f, buf2, sizeof(buf2));
    file_put(f);
    return got == (int64_t)n && memcmp(buf2, text, n) == 0;
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

char lower(char c){
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

// name apare in lista, exact sau (nocase) fara diferenta de majuscule ASCII
bool listed(const char* name, bool nocase = false){
    uint64_t nl = strlen_(name);
    for(const char* p = names; *p; p++){
        if(*p != ',' || p[1] == 0){
            continue;
        }
        uint64_t k = 0;
        while(k < nl && p[1 + k] && (nocase ? lower(p[1 + k]) == lower(name[k]) : p[1 + k] == name[k])){
            k++;
        }
        if(k == nl && p[1 + k] == ','){
            return true;
        }
    }
    return false;
}

uint32_t free_clusters(){
    File* f = open_ok("/media", O_RDONLY | O_DIRECTORY);
    uint32_t n = fat32_free_clusters(f->inode);
    file_put(f);
    return n;
}

// ---- citire dintr-o imagine facuta de mkfs.fat + mtools ----

void check_read(){
    CHECK(text_is("/media/HELLO.TXT", "hello from mkfs.fat\n"));
    CHECK(text_is("/media/hello.txt", "hello from mkfs.fat\n"));
    CHECK(text_is("/media/Long File Name Example.txt", "long name via mtools\n"));
    CHECK(text_is("/media/LONG FILE name example.TXT", "long name via mtools\n"));
    CHECK(text_is("/media/dir1/nested.txt", "nested file\n"));
    CHECK(text_is("/media/Mixed Case Dir/deep file name.txt", "deep\n"));
    Stat st;
    EQ(vfs_stat(none, "/media/DIR1", true, st), 0);
    CHECK(S_ISDIR(st.st_mode));
    EQ(vfs_stat(none, "/media/hello.txt", true, st), 0);
    EQ(st.st_size, 20);
    CHECK(S_ISREG(st.st_mode));
    CHECK(st.st_mtim.sec > 1700000000);             // data din 2023+: timpul FAT s-a decodat

    int n = list_dir("/media");
    CHECK(n >= 5);
    CHECK(listed("HELLO.TXT", true) && listed("Long File Name Example.txt") && listed("dir1", true) &&
          listed("big.bin", true) && listed("Mixed Case Dir"));

    // big.bin: lant de multe clustere, citit cu bucati nealiniate, apoi cu seek
    EQ(size_of("/media/big.bin"), BIG_SIZE);
    File* f = open_ok("/media/big.bin", O_RDONLY);
    check_pat(f, 0, BIG_SIZE, 0);
    uint64_t offs[] = {0, 1, 511, 512, 513, 4095, 4096, 65535, 65536, 123457, BIG_SIZE - 1};
    for(uint64_t o : offs){
        seek(f, o);
        uint8_t b;
        EQ(rd(f, &b, 1), 1);
        CHECK(b == pat(o, 0));
    }
    seek(f, BIG_SIZE);
    EQ(rd(f, buf2, 10), 0);
    seek(f, 299990);
    EQ(rd(f, buf2, 100), 10);
    file_put(f);

    EQ(open_err("/media/nu_exista.txt", O_RDONLY), -ENOENT);
    EQ(open_err("/media/HELLO.TXT/x", O_RDONLY), -ENOTDIR);
    EQ(open_err("/media/dir1", O_WRONLY), -EISDIR);
    EQ(open_err("/media/dir1", O_RDONLY | O_DIRECTORY), 0);
    EQ(vfs_symlink(none, "x", "/media/lnk"), -EPERM);
    kprintf("[OK] test_fat32 citire (BPB, 8.3, LFN, lanturi, nume fara majuscule)\n");
}

// ---- scriere, extindere, trunchiere ----

void check_write(){
    put_file("/media/new.txt", 5000, 1);
    verify_file("/media/new.txt", 5000, 1);

    File* f = open_ok("/media/new.txt", O_WRONLY | O_APPEND);
    write_pat(f, 5000, 3000, 1);
    file_put(f);
    verify_file("/media/new.txt", 8000, 1);

    f = open_ok("/media/new.txt", O_RDWR);
    seek(f, 1000);
    write_pat(f, 1000, 600, 9);
    seek(f, 0);
    check_part(f, 0, 1000, 1, 1000);
    file_put(f);
    f = open_ok("/media/new.txt", O_RDONLY);
    seek(f, 1000);
    for(uint64_t i = 0; i < 600; i++){
        uint8_t b;
        EQ(rd(f, &b, 1), 1);
        CHECK(b == pat(1000 + i, 9));
    }
    EQ(rd(f, buf2, 5), 5);
    CHECK(buf2[0] == pat(1600, 1));
    file_put(f);

    // Trunchiere: scade, apoi creste cu zerouri (inclusiv peste coada veche a clusterului)
    f = open_ok("/media/new.txt", O_RDWR);
    uint32_t before = free_clusters();
    EQ(f->inode->ops->truncate(f->inode, 100), 0);
    CHECK(free_clusters() > before);
    EQ(size_of("/media/new.txt"), 100);
    EQ(f->inode->ops->truncate(f->inode, 9000), 0);
    seek(f, 100);
    check_zero(f, 8900);
    seek(f, 0);
    EQ(rd(f, buf2, 100), 100);
    CHECK(buf2[0] == pat(0, 1) && buf2[99] == pat(99, 1));
    // Scriere dincolo de sfarsit: golul se citeste ca zerouri
    seek(f, 20000);
    EQ(wr(f, "tail", 4), 4);
    EQ(size_of("/media/new.txt"), 20004);
    seek(f, 9000);
    check_zero(f, 11000);
    file_put(f);

    // O_TRUNC, O_EXCL
    EQ(open_err("/media/new.txt", O_CREAT | O_EXCL | O_WRONLY), -EEXIST);
    f = open_ok("/media/new.txt", O_WRONLY | O_TRUNC);
    file_put(f);
    EQ(size_of("/media/new.txt"), 0);
    put_text("/media/new.txt", "dupa trunchiere\n");
    CHECK(text_is("/media/new.txt", "dupa trunchiere\n"));
    kprintf("[OK] test_fat32 scriere (extindere, trunchiere, goluri de zerouri)\n");
}

// ---- nume: 8.3 cu minuscule NT, LFN, Unicode, aliasuri ~N, caractere interzise ----

void check_names(){
    EQ(vfs_mkdir(none, "/media/names", 0755), 0);
    const char* made[] = {"lower.txt", "UPPER.TXT", "Mixed.Txt", "a b.txt", "verylongname.extension",
                          "verylongname2.extension", "noext", ".hidden", "two.dots.txt", "x",
                          "\xC4\x83\xC3\xA2\xC3\xAE\xC8\x99\xC8\x9B lung.txt",       // ăâîșț lung.txt
                          "smile\xF0\x9F\x98\x80.txt",                                  // 4 octeti UTF-8
                          "Sub\xC8\x9Bire \xC8\x98" "oapt\xC4\x83.txt"};
    int count = (int)(sizeof(made) / sizeof(made[0]));
    char path[PATH_MAX];
    for(int i = 0; i < count; i++){
        uint64_t n = strlen_("/media/names/");
        memcpy(path, "/media/names/", n);
        uint64_t l = strlen_(made[i]);
        memcpy(path + n, made[i], l + 1);
        put_text(path, made[i]);
    }
    EQ(list_dir("/media/names"), count);
    for(int i = 0; i < count; i++){
        if(!listed(made[i])){
            panic("test_fat32: lipseste din lista: %s", made[i]);
        }
        uint64_t n = strlen_("/media/names/");
        memcpy(path, "/media/names/", n);
        memcpy(path + n, made[i], strlen_(made[i]) + 1);
        CHECK(text_is(path, made[i]));
    }
    // Aliasuri generate si cautare fara diferenta de majuscule (inclusiv litere romanesti)
    CHECK(text_is("/media/names/VERYLO~1.EXT", "verylongname.extension"));
    CHECK(text_is("/media/names/VERYLO~2.EXT", "verylongname2.extension"));
    CHECK(text_is("/media/names/LOWER.TXT", "lower.txt"));
    CHECK(text_is("/media/names/upper.txt", "UPPER.TXT"));
    CHECK(text_is("/media/names/mixed.TXT", "Mixed.Txt"));
    CHECK(text_is("/media/names/\xC4\x82\xC3\x82\xC3\x8E\xC8\x98\xC8\x9A LUNG.TXT", made[10]));
    CHECK(text_is("/media/names/SUB\xC8\x9AIRE \xC8\x99OAPT\xC4\x82.TXT", made[12]));
    EQ(open_err("/media/names/Mixed.Txt", O_CREAT | O_EXCL | O_WRONLY), -EEXIST);
    EQ(open_err("/media/names/MIXED.TXT", O_CREAT | O_EXCL | O_WRONLY), -EEXIST);

    // Lungimi si caractere interzise
    char longname[300];
    memcpy(longname, "/media/names/", 13);
    memset(longname + 13, 'n', 255);
    longname[13 + 255] = 0;
    put_text(longname, "255");
    CHECK(text_is(longname, "255"));
    EQ(vfs_unlink(none, longname), 0);
    longname[13 + 255] = 'n';
    longname[13 + 256] = 0;
    EQ(open_err(longname, O_CREAT | O_WRONLY), -ENAMETOOLONG);
    EQ(open_err("/media/names/a:b", O_CREAT | O_WRONLY), -EINVAL);
    EQ(open_err("/media/names/a*b", O_CREAT | O_WRONLY), -EINVAL);
    EQ(open_err("/media/names/q?", O_CREAT | O_WRONLY), -EINVAL);
    EQ(open_err("/media/names/a|b", O_CREAT | O_WRONLY), -EINVAL);
    EQ(open_err("/media/names/ends.", O_CREAT | O_WRONLY), -EINVAL);
    EQ(open_err("/media/names/bad\xFF" "utf8", O_CREAT | O_WRONLY), -EINVAL);
    EQ(list_dir("/media/names"), count);

    for(int i = 0; i < count; i++){
        uint64_t n = strlen_("/media/names/");
        memcpy(path, "/media/names/", n);
        memcpy(path + n, made[i], strlen_(made[i]) + 1);
        if(i == 10 || i == 12){
            continue;                               // raman pe disc pentru verificarea gazdei
        }
        EQ(vfs_unlink(none, path), 0);
    }
    EQ(list_dir("/media/names"), 2);
    kprintf("[OK] test_fat32 nume (LFN, Unicode, aliasuri ~N, interzise)\n");
}

// ---- un director care creste peste multe clustere ----

void check_many(){
    EQ(vfs_mkdir(none, "/media/many", 0755), 0);
    char path[64];
    const int N = 60;
    auto make_path = [&](int i){
        memcpy(path, "/media/many/file_00_long_name_for_lfn.dat", 42);
        path[17] = (char)('0' + i / 10);
        path[18] = (char)('0' + i % 10);
    };
    for(int i = 0; i < N; i++){
        make_path(i);
        put_file(path, 100 + (uint64_t)i * 37, (uint8_t)i);
    }
    EQ(list_dir("/media/many"), N);
    for(int i = 0; i < N; i++){
        make_path(i);
        verify_file(path, 100 + (uint64_t)i * 37, (uint8_t)i);
    }
    for(int i = 1; i < N; i += 2){
        make_path(i);
        EQ(vfs_unlink(none, path), 0);
    }
    EQ(list_dir("/media/many"), N / 2);
    for(int i = 1; i < N; i += 2){                  // intrarile sterse se refolosesc
        make_path(i);
        put_file(path, 50, (uint8_t)(i + 100));
    }
    EQ(list_dir("/media/many"), N);
    for(int i = 0; i < N; i++){
        make_path(i);
        if(i & 1){
            verify_file(path, 50, (uint8_t)(i + 100));
        } else {
            verify_file(path, 100 + (uint64_t)i * 37, (uint8_t)i);
        }
    }
    EQ(vfs_rmdir(none, "/media/many"), -ENOTEMPTY);
    for(int i = 0; i < N; i++){
        make_path(i);
        EQ(vfs_unlink(none, path), 0);
    }
    EQ(list_dir("/media/many"), 0);
    EQ(vfs_rmdir(none, "/media/many"), 0);
    EQ(open_err("/media/many", O_RDONLY), -ENOENT);
    kprintf("[OK] test_fat32 directoare mari (%d fisiere, extindere de director, refolosire de intrari)\n", N);
}

// ---- mkdir / rmdir / unlink / rename ----

void check_dirs(){
    EQ(vfs_mkdir(none, "/media/a", 0755), 0);
    EQ(vfs_mkdir(none, "/media/a/b", 0755), 0);
    EQ(vfs_mkdir(none, "/media/a/b/c", 0755), 0);
    EQ(vfs_mkdir(none, "/media/a", 0755), -EEXIST);
    EQ(vfs_mkdir(none, "/media/A", 0755), -EEXIST);
    put_text("/media/a/b/c/f.txt", "in c\n");
    EQ(vfs_rmdir(none, "/media/a"), -ENOTEMPTY);
    EQ(vfs_rmdir(none, "/media/a/b/c"), -ENOTEMPTY);
    EQ(vfs_unlink(none, "/media/a"), -EISDIR);
    EQ(vfs_rmdir(none, "/media/a/b/c/f.txt"), -ENOTDIR);

    // Mutare in sus, apoi mutarea unui director intreg in radacina cu nume lung
    EQ(vfs_rename(none, "/media/a/b/c/f.txt", "/media/a/f2.txt"), 0);
    EQ(open_err("/media/a/b/c/f.txt", O_RDONLY), -ENOENT);
    CHECK(text_is("/media/a/f2.txt", "in c\n"));
    EQ(vfs_rename(none, "/media/a/b", "/media/Renamed Directory"), 0);
    EQ(open_err("/media/a/b", O_RDONLY | O_DIRECTORY), -ENOENT);
    put_text("/media/Renamed Directory/c/g.txt", "after move\n");
    CHECK(text_is("/media/Renamed Directory/c/g.txt", "after move\n"));
    EQ(list_dir("/media/Renamed Directory"), 1);
    CHECK(listed("c"));

    // Reguli de tip si cicluri
    put_text("/media/r1.txt", "r1");
    put_text("/media/r2.txt", "r2");
    EQ(vfs_rename(none, "/media/r1.txt", "/media/Renamed Directory"), -EISDIR);
    EQ(vfs_rename(none, "/media/Renamed Directory", "/media/r1.txt"), -ENOTDIR);
    EQ(vfs_rename(none, "/media/Renamed Directory", "/media/Renamed Directory/c/inside"), -EINVAL);
    EQ(vfs_rename(none, "/media/Renamed Directory", "/media/a"), -ENOTEMPTY);
    EQ(vfs_rename(none, "/media/nu_exista", "/media/x"), -ENOENT);
    EQ(vfs_rename(none, "/media/r1.txt", "/media/r1.txt"), 0);
    EQ(vfs_rename(none, "/media/r1.txt", "/tmp/r1.txt"), -EXDEV);
    EQ(vfs_rename(none, "/media/r1.txt", "/media/bad:name"), -EINVAL);
    CHECK(text_is("/media/r1.txt", "r1"));

    // Fisier peste fisier: il inlocuieste
    uint32_t free0 = free_clusters();
    put_file("/media/victim.bin", 9000, 3);
    CHECK(free_clusters() < free0);
    EQ(vfs_rename(none, "/media/r1.txt", "/media/victim.bin"), 0);
    CHECK(text_is("/media/victim.bin", "r1"));
    EQ(open_err("/media/r1.txt", O_RDONLY), -ENOENT);
    EQ(free_clusters(), free0);                     // victima si-a eliberat clusterele
    // Director gol peste director gol
    EQ(vfs_mkdir(none, "/media/e1", 0755), 0);
    EQ(vfs_mkdir(none, "/media/e2", 0755), 0);
    EQ(vfs_rename(none, "/media/e1", "/media/e2"), 0);
    EQ(open_err("/media/e1", O_RDONLY | O_DIRECTORY), -ENOENT);
    EQ(vfs_rmdir(none, "/media/e2"), 0);

    // Doar litere mari / mici: acelasi fisier, alt nume pe disc; scurt <-> lung
    EQ(vfs_rename(none, "/media/new.txt", "/media/NEW.TXT"), 0);
    EQ(list_dir("/media") > 0, 1);
    CHECK(listed("NEW.TXT") && !listed("new.txt"));
    CHECK(text_is("/media/new.txt", "dupa trunchiere\n"));
    EQ(vfs_rename(none, "/media/NEW.TXT", "/media/Un Nume Lung Pentru Fisier.txt"), 0);
    CHECK(text_is("/media/Un Nume Lung Pentru Fisier.txt", "dupa trunchiere\n"));
    EQ(vfs_rename(none, "/media/Un Nume Lung Pentru Fisier.txt", "/media/short.txt"), 0);
    CHECK(text_is("/media/SHORT.TXT", "dupa trunchiere\n"));

    // Fisier deschis: redenumit si apoi sters cat timp e deschis; datele raman accesibile prin descriptor
    File* f = open_ok("/media/short.txt", O_RDWR);
    EQ(vfs_rename(none, "/media/short.txt", "/media/moved while open.txt"), 0);
    seek(f, 0);
    EQ(rd(f, buf2, 5), 5);
    CHECK(memcmp(buf2, "dupa ", 5) == 0);
    EQ(vfs_unlink(none, "/media/moved while open.txt"), 0);
    uint32_t free1 = free_clusters();
    EQ(f->inode->ops->truncate(f->inode, 5000), 0);
    CHECK(free_clusters() < free1);
    seek(f, 4000);
    EQ(wr(f, "zzz", 3), 3);
    seek(f, 4000);
    EQ(rd(f, buf2, 3), 3);
    CHECK(memcmp(buf2, "zzz", 3) == 0);
    file_put(f);
    EQ(free_clusters(), free1 + 1);                 // la ultimul close se elibereaza tot lantul fisierului sters
    EQ(open_err("/media/short.txt", O_RDONLY), -ENOENT);

    // Curatenie; ramane /media/Renamed Directory/c/g.txt si /media/a/f2.txt pentru fsck
    EQ(vfs_unlink(none, "/media/victim.bin"), 0);
    EQ(vfs_unlink(none, "/media/r2.txt"), 0);
    EQ(vfs_rename(none, "/media/a/f2.txt", "/media/Renamed Directory/f2.txt"), 0);
    EQ(vfs_rmdir(none, "/media/a"), 0);
    kprintf("[OK] test_fat32 mkdir / rmdir / unlink / rename (peste, intre directoare, deschis)\n");
}

// ---- fisiere mari: extindere pe multe clustere, lant ----

void check_grow(){
    uint32_t free0 = free_clusters();
    const uint64_t BIG = 2u << 20;
    File* f = open_ok("/media/grow.bin", O_CREAT | O_RDWR);
    write_pat(f, 0, BIG, 5);
    file_put(f);
    uint32_t used = free0 - free_clusters();
    CHECK(used >= BIG / 4096);
    verify_file("/media/grow.bin", BIG, 5);
    f = open_ok("/media/grow.bin", O_RDWR);
    EQ(f->inode->ops->truncate(f->inode, (1u << 20) + 17), 0);
    file_put(f);
    verify_file("/media/grow.bin", (1u << 20) + 17, 5);
    CHECK(free_clusters() > free0 - used);
    EQ(vfs_unlink(none, "/media/grow.bin"), 0);
    EQ(free_clusters(), free0);
    kprintf("[OK] test_fat32 extindere (2 MiB, trunchiere, clustere eliberate: %u)\n", free_clusters());
}

// ---- volum plin: scriere pana la ENOSPC, apoi totul se elibereaza ----

void check_full(){
    uint32_t free0 = free_clusters();
    File* f = open_ok("/media/full.bin", O_CREAT | O_WRONLY);
    for(uint32_t i = 0; i < sizeof(buf); i++){
        buf[i] = (uint8_t)(i * 7);
    }
    uint64_t total = 0;
    int64_t r;
    while((r = wr(f, buf, sizeof(buf))) > 0){
        total += (uint64_t)r;
    }
    EQ(r, -ENOSPC);
    EQ(free_clusters(), 0);
    CHECK(total > 60u << 20);
    EQ(size_of("/media/full.bin"), total);
    EQ(vfs_mkdir(none, "/media/nospace", 0755), -ENOSPC);
    EQ(open_err("/media/nospace", O_RDONLY), -ENOENT);
    EQ(wr(f, buf, 10), -ENOSPC);
    EQ(size_of("/media/full.bin"), total);
    file_put(f);
    f = open_ok("/media/full.bin", O_RDONLY);
    seek(f, total - 5000);
    EQ(rd(f, buf2, 5000), 5000);
    for(uint64_t i = 0; i < 5000; i++){
        CHECK(buf2[i] == (uint8_t)(((total - 5000 + i) % sizeof(buf)) * 7));
    }
    file_put(f);
    EQ(vfs_unlink(none, "/media/full.bin"), 0);
    EQ(free_clusters(), free0);
    kprintf("[OK] test_fat32 volum plin (%lu KiB scrisi pana la ENOSPC, apoi eliberati)\n", total >> 10);
}

// ---- marcaje pentru gazda: ramase pe disc pentru fsck.fat si mtools ----

void leave_marks(){
    put_text("/media/PERSIST.TXT", "scris de YokaiOS\n");
    put_file("/media/persist.bin", 70000, 7);
    EQ(vfs_mkdir(none, "/media/Kept Directory", 0755), 0);
    put_text("/media/Kept Directory/kept file.txt", "kept\n");
}

void check_persist(){
    CHECK(text_is("/media/PERSIST.TXT", "scris de YokaiOS\n"));
    verify_file("/media/persist.bin", 70000, 7);
    CHECK(text_is("/media/Kept Directory/kept file.txt", "kept\n"));
    CHECK(text_is("/media/names/\xC4\x83\xC3\xA2\xC3\xAE\xC8\x99\xC8\x9B lung.txt", "\xC4\x83\xC3\xA2\xC3\xAE\xC8\x99\xC8\x9B lung.txt"));
    CHECK(text_is("/media/Renamed Directory/c/g.txt", "after move\n"));
    CHECK(text_is("/media/Renamed Directory/f2.txt", "in c\n"));
    CHECK(text_is("/media/hello.txt", "hello from mkfs.fat\n"));
    File* f = open_ok("/media/big.bin", O_RDONLY);
    check_pat(f, 0, BIG_SIZE, 0);
    file_put(f);
    EQ(open_err("/media/a", O_RDONLY), -ENOENT);
}

struct Job {
    volatile bool done;
};

void fat32_task(void* arg){
    Job* j = (Job*)arg;
    // Montat la boot de fat32_automount
    CHECK(exists("/media/HELLO.TXT"));
    kprintf("[OK] test_fat32 automount\n");
    uint32_t free_start = free_clusters();
    check_read();
    check_write();
    check_names();
    check_many();
    check_dirs();
    check_grow();
    check_full();
    leave_marks();
    EQ(bsync(nullptr), 0);

    // Demontare (scrie FSInfo si bitul "curat"), apoi montare din nou de pe disc si aceleasi verificari
    uint32_t free_end = free_clusters();
    EQ(vfs_umount(none, "/media"), 0);
    EQ(open_err("/media/HELLO.TXT", O_RDONLY), -ENOENT);
    EQ(vfs_mount(none, "sda1", "/media", "fat32", 0, nullptr), 0);
    check_persist();
    EQ(free_clusters(), free_end);
    CHECK(free_end < free_start);
    // Montat read-only: scrierile esueaza
    EQ(vfs_umount(none, "/media"), 0);
    EQ(vfs_mount(none, "sda1", "/media", "fat32", 0, "ro"), 0);
    EQ(open_err("/media/HELLO.TXT", O_WRONLY), -EROFS);
    EQ(vfs_mkdir(none, "/media/nu", 0755), -EROFS);
    EQ(vfs_unlink(none, "/media/HELLO.TXT"), -EROFS);
    check_persist();
    EQ(vfs_umount(none, "/media"), 0);
    EQ(vfs_mount(none, "sda1", "/media", "fat32", 0, "ro,foo"), -EINVAL);
    EQ(vfs_mount(none, "nu_exista", "/media", "fat32", 0, nullptr), -ENODEV);
    kprintf("[OK] test_fat32 remontare (date si numar de clustere libere pastrate)\n");
    j->done = true;
}

}

void test_fat32_run(){
    Job job = {false};
    Task* t = task_create_kernel("t-fat32", fat32_task, &job);
    CHECK(t);
    sched_add(t);
    // La boot intreruperile sunt oprite, iar hlt-ul din sleep_ms le cere pornite
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 2400 && !job.done; i++){
        sleep_ms(50);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    kprintf("[OK] test_fat32 (FAT32: BPB, FAT cache, lanturi, LFN citire si scriere, creare / stergere / redenumire, "
            "extindere, remontare)\n");
}
