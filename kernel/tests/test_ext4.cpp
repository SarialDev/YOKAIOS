#include "tests/test_ext4.h"
#include "fs/ext4/ext4.h"
#include "fs/pseudofs.h"
#include "fs/vfs/vfs.h"
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
        if(got_ != want_){ panic("test_ext4:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_ext4:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};

struct Job {
    int mode;                       // 0 ok, 1 refuse, 2 recover
    volatile bool done;
};

uint8_t fbuf[65536];
uint8_t fbuf2[65536];
char path[1024];
uint32_t crc_table[256];
uint32_t listed = 0;

uint64_t strlen_(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

bool streq(const char* a, const char* b){
    return strlen_(a) == strlen_(b) && memcmp(a, b, strlen_(a)) == 0;
}

uint8_t pat(uint64_t off){
    return (uint8_t)(off * 131 + (off >> 9));
}

void crc_init(){
    for(uint32_t i = 0; i < 256; i++){
        uint32_t c = i;
        for(int k = 0; k < 8; k++){
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[i] = c;
    }
}

uint32_t crc_update(uint32_t crc, const uint8_t* p, uint64_t n){
    while(n--){
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

File* open_ok(const Path& base, const char* p, int flags = O_RDONLY){
    File* f;
    EQ(vfs_open(base, p, flags, 0, f), 0);
    return f;
}

int open_err(const char* p, int flags){
    File* f;
    int err = vfs_open(none, p, flags, 0644, f);
    if(!err){
        file_put(f);
    }
    return err;
}

int64_t rd(File* f, void* b, uint64_t n){
    return f->ops->read(f, b, n, false);
}

void seek(File* f, uint64_t pos){
    EQ(f->ops->lseek(f, (int64_t)pos, SEEK_SET), (int64_t)pos);
}

uint64_t size_of(const char* p){
    Stat st;
    EQ(vfs_stat(none, p, true, st), 0);
    return (uint64_t)st.st_size;
}

bool text_is(const char* p, const char* text){
    File* f;
    if(vfs_open(none, p, O_RDONLY, 0, f)){
        return false;
    }
    uint64_t n = strlen_(text);
    int64_t got = rd(f, fbuf, sizeof(fbuf));
    file_put(f);
    return got == (int64_t)n && memcmp(fbuf, text, n) == 0;
}

// De la pozitia curenta: len octeti cu modelul pat(offset absolut), in bucati de chunk
void check_pat(File* f, uint64_t from, uint64_t len, uint32_t chunk){
    uint64_t done = 0;
    while(done < len){
        uint64_t n = len - done < chunk ? len - done : chunk;
        EQ(rd(f, fbuf2, n), (int64_t)n);
        for(uint64_t i = 0; i < n; i++){
            if(fbuf2[i] != pat(from + done + i)){
                panic("test_ext4: octet gresit la %lu (%x, asteptat %x)", from + done + i, fbuf2[i],
                      pat(from + done + i));
            }
        }
        done += n;
    }
}

void check_zero(File* f, uint64_t len){
    while(len){
        uint64_t n = len < sizeof(fbuf2) ? len : sizeof(fbuf2);
        EQ(rd(f, fbuf2, n), (int64_t)n);
        for(uint64_t i = 0; i < n; i++){
            CHECK(fbuf2[i] == 0);
        }
        len -= n;
    }
}

// ---- parcurgerea arborelui: o linie "EXT4 ..." pentru fiecare intrare, comparata de gazda cu sursa ----

void walk(const Path& dp, uint32_t plen, uint32_t depth){
    CHECK(depth < 8);
    File* d = open_ok(dp, ".", O_RDONLY | O_DIRECTORY);
    uint8_t dbuf[1536];
    int64_t got;
    while((got = d->ops->getdents(d, dbuf, sizeof(dbuf))) > 0){
        for(int64_t off = 0; off < got;){
            Dirent64* e = (Dirent64*)(dbuf + off);
            off += e->d_reclen;
            const char* name = e->d_name;
            if(name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))){
                continue;
            }
            uint64_t nl = strlen_(name);
            CHECK(plen + 1 + nl < sizeof(path));
            path[plen] = '/';
            memcpy(path + plen + 1, name, nl + 1);
            uint32_t np = (uint32_t)(plen + 1 + nl);
            Stat st;
            EQ(vfs_stat(dp, name, false, st), 0);
            uint32_t mode = st.st_mode;
            listed++;
            if(S_ISDIR(mode)){
                kprintf("EXT4 D %04x %s\n", mode & 07777, path);
                Path child;
                vfs_path_copy(child, dp);
                EQ(vfs_chdir(child, name), 0);
                walk(child, np, depth + 1);
                vfs_path_release(child);
            } else if(S_ISREG(mode)){
                File* f = open_ok(dp, name);
                static const uint32_t chunks[] = {4096, 1000, 65536, 4097, 512, 32768};
                uint32_t chunk = chunks[listed % 6];
                uint32_t crc = 0xFFFFFFFFu;
                uint64_t total = 0;
                int64_t r;
                while((r = rd(f, fbuf, chunk)) > 0){
                    crc = crc_update(crc, fbuf, (uint64_t)r);
                    total += (uint64_t)r;
                }
                EQ(r, 0);
                file_put(f);
                EQ(total, st.st_size);
                kprintf("EXT4 F %08x %lu %04x %s\n", ~crc, total, mode & 07777, path);
            } else if(S_ISLNK(mode)){
                char tgt[300];
                int64_t n = vfs_readlink(dp, name, tgt, sizeof(tgt) - 1);
                CHECK(n >= 0);
                EQ(n, st.st_size);
                tgt[n] = 0;
                kprintf("EXT4 L %s -> %s\n", path, tgt);
            } else if((mode & S_IFMT) == S_IFIFO){
                kprintf("EXT4 P %04x %s\n", mode & 07777, path);
            } else {
                kprintf("EXT4 ? %06o %s\n", mode, path);
            }
            path[plen] = 0;
        }
    }
    EQ(got, 0);
    file_put(d);
}

uint32_t count_dir(const char* p){
    File* f = open_ok(none, p, O_RDONLY | O_DIRECTORY);
    uint8_t dbuf[1024];
    uint32_t n = 0;
    int64_t got;
    while((got = f->ops->getdents(f, dbuf, sizeof(dbuf))) > 0){
        for(int64_t off = 0; off < got;){
            Dirent64* e = (Dirent64*)(dbuf + off);
            off += e->d_reclen;
            if(!(e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))){
                n++;
            }
        }
    }
    EQ(got, 0);
    file_put(f);
    return n;
}

void put_name(char* out, const char* prefix, uint32_t n, const char* suffix){
    uint64_t k = strlen_(prefix);
    memcpy(out, prefix, k);
    for(int i = 4; i >= 0; i--){
        out[k + i] = (char)('0' + n % 10);
        n /= 10;
    }
    k += 5;
    uint64_t sl = strlen_(suffix);
    memcpy(out + k, suffix, sl + 1);
}

// Toate cele count nume prefix%05u suffix exista (cautate una cate una), cele de dincolo nu
void check_big_dir(const char* dir, const char* prefix, const char* suffix, uint32_t count, bool indexed){
    EQ(count_dir(dir), count);
    File* df = open_ok(none, dir, O_RDONLY | O_DIRECTORY);
    CHECK(ext4_dir_indexed(df->inode) == indexed);
    file_put(df);
    uint64_t h0, l0;
    ext4_stats(h0, l0);
    char p[160];
    uint64_t dl = strlen_(dir);
    memcpy(p, dir, dl);
    p[dl] = '/';
    for(uint32_t i = 0; i < count; i++){
        put_name(p + dl + 1, prefix, i, suffix);
        Stat st;
        EQ(vfs_stat(none, p, true, st), 0);
        CHECK(S_ISREG(st.st_mode));
    }
    put_name(p + dl + 1, prefix, count, suffix);
    EQ(open_err(p, O_RDONLY), -ENOENT);
    put_name(p + dl + 1, prefix, 99999, suffix);
    EQ(open_err(p, O_RDONLY), -ENOENT);
    put_name(p + dl + 1, prefix, 0, "x");
    EQ(open_err(p, O_RDONLY), -ENOENT);
    uint64_t h1, l1;
    ext4_stats(h1, l1);
    if(indexed){
        CHECK(h1 - h0 >= count);
        CHECK(l1 == l0);                        // nicio cautare liniara: indexul a gasit tot
    } else {
        CHECK(l1 - l0 >= count);
        CHECK(h1 == h0);
    }
}

// size_N: fisier cu modelul pat, dimensiune N (praguri de blocuri / indirectari)
void check_sizes(){
    static const uint64_t sizes[] = {1, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 12 * 1024 - 1,
                                     12 * 1024, 12 * 1024 + 1, 268 * 1024 - 1, 268 * 1024, 268 * 1024 + 1, 1000000};
    for(uint64_t s : sizes){
        char p[64];
        memcpy(p, "/data/sizes/size_", 17);
        uint64_t n = 17;
        char digits[24];
        int dn = 0;
        for(uint64_t x = s; x; x /= 10){
            digits[dn++] = (char)('0' + x % 10);
        }
        while(dn){
            p[n++] = digits[--dn];
        }
        p[n] = 0;
        EQ(size_of(p), s);
        File* f = open_ok(none, p);
        check_pat(f, 0, s, 3001);
        EQ(rd(f, fbuf2, 16), 0);
        // Acces aleator: ultimii octeti si un punct din mijloc
        if(s > 100){
            seek(f, s - 50);
            check_pat(f, s - 50, 50, 17);
            seek(f, s / 2);
            check_pat(f, s / 2, 40, 40);
        }
        EQ(f->ops->lseek(f, 0, SEEK_END), (int64_t)s);
        file_put(f);
    }
}

void check_sparse(){
    static const uint64_t islands[] = {0, 1u << 20, 33u << 20, 65u << 20, 66u << 20};
    const uint64_t total = (66u << 20) + 4096;
    EQ(size_of("/data/sparse.bin"), total);
    File* f = open_ok(none, "/data/sparse.bin");
    for(uint64_t o : islands){
        if(o){
            seek(f, o - 300);
            check_zero(f, 300);
        }
        seek(f, o);
        check_pat(f, o, 4096, 1500);
        if(o + 4096 < total){
            check_zero(f, 700);
        }
    }
    // Un goluri mari se citeste ca zerouri, pe mai multe blocuri deodata
    seek(f, 2u << 20);
    check_zero(f, 8u << 20);
    EQ(f->ops->lseek(f, 0, SEEK_END), (int64_t)total);
    EQ(rd(f, fbuf2, 10), 0);
    file_put(f);
}

void check_islands(){
    // 1500 insule de 512 octeti la i * 8192: multe extente (arbore pe 2+ niveluri) sau harta de blocuri
    EQ(size_of("/data/islands.bin"), 1499ull * 8192 + 512);
    File* f = open_ok(none, "/data/islands.bin");
    for(uint32_t i = 0; i < 1500; i += (i < 20 ? 1 : 37)){
        uint64_t o = (uint64_t)i * 8192;
        seek(f, o);
        check_pat(f, o, 512, 512);
        if(i < 1499){
            check_zero(f, 700);
        }
    }
    file_put(f);
}

void check_data(const char* variant, bool indexed){
    CHECK(text_is("/data/hello.txt", "hello from ext4\n"));
    EQ(size_of("/data/empty"), 0);
    // symlink-uri: scurt (in inode), lung (>60, in bloc), absolut spre director, bucla, gol
    CHECK(text_is("/data/link_short", "hello from ext4\n"));
    CHECK(text_is("/data/link_long", "long target\n"));
    CHECK(text_is("/data/link_dir/hello.txt", "hello from ext4\n"));
    char tgt[300];
    EQ(vfs_readlink(none, "/data/link_short", tgt, sizeof(tgt)), 9);
    CHECK(memcmp(tgt, "hello.txt", 9) == 0);
    EQ(vfs_readlink(none, "/data/link_long", tgt, sizeof(tgt)), 88);
    EQ(open_err("/data/loop_a", O_RDONLY), -ELOOP);
    EQ(open_err("/data/link_dangling", O_RDONLY), -ENOENT);
    Stat st;
    EQ(vfs_stat(none, "/data/link_short", false, st), 0);
    CHECK(S_ISLNK(st.st_mode));
    // legaturi dure
    Stat a, b;
    EQ(vfs_stat(none, "/data/hard_a", true, a), 0);
    EQ(vfs_stat(none, "/data/hard_b", true, b), 0);
    CHECK(a.st_ino == b.st_ino);
    EQ(a.st_nlink, 2);
    // atribute
    EQ(vfs_stat(none, "/data/hello.txt", true, st), 0);
    EQ(st.st_size, 16);
    EQ(st.st_mode, S_IFREG | 0644);
    CHECK(st.st_mtim.sec > 1500000000);
    EQ(vfs_stat(none, "/data", true, st), 0);
    CHECK(S_ISDIR(st.st_mode));
    EQ(open_err("/data/hello.txt/x", O_RDONLY), -ENOTDIR);
    EQ(open_err("/data/nu_exista", O_RDONLY), -ENOENT);
    CHECK(open_err("/data", O_WRONLY) < 0);

    check_sizes();
    {
        // big.bin: 5 MiB cu modelul pat, citit in bucati neobisnuite
        EQ(size_of("/data/big.bin"), 5u << 20);
        File* f = open_ok(none, "/data/big.bin");
        check_pat(f, 0, 5u << 20, 4093);
        seek(f, 1234567);
        check_pat(f, 1234567, 100000, 777);
        file_put(f);
    }
    kprintf("[OK] test_ext4 fisiere (dimensiuni la pragurile de blocuri / indirectari, big.bin, symlink-uri, link-uri dure)\n");
    check_sparse();
    kprintf("[OK] test_ext4 sparse (goluri, indirectare dubla / tripla sau extente)\n");
    check_islands();
    kprintf("[OK] test_ext4 insule (arbore de extente adanc / harta de blocuri)\n");
    EQ(count_dir("/longname"), 1);
    EQ(count_dir("/unicode"), 1);
    check_big_dir("/many", "f", "", 3000, indexed);
    check_big_dir("/huge", "huge_entry_", "_pad", 9000, indexed);
    kprintf("[OK] test_ext4 directoare mari (%s: %s)\n", variant, indexed ? "htree" : "liniar");
}

void check_readonly(){
    EQ(vfs_mkdir(none, "/data/nou", 0755), -EROFS);
    EQ(vfs_unlink(none, "/data/hello.txt"), -EROFS);
    EQ(vfs_rmdir(none, "/data/sizes"), -EROFS);
    EQ(vfs_rename(none, "/data/hello.txt", "/data/hello2.txt"), -EROFS);
    EQ(vfs_symlink(none, "x", "/data/sl"), -EROFS);
    int err = open_err("/data/nou_fisier", O_CREAT | O_WRONLY);
    CHECK(err == -EROFS);
    CHECK(open_err("/data/hello.txt", O_WRONLY) < 0);
    CHECK(text_is("/data/hello.txt", "hello from ext4\n"));
    kprintf("[OK] test_ext4 read-only (-EROFS la orice scriere)\n");
}

// Prima montare din arbore: tipul si numele dispozitivului
void root_mount(char* type, char* dev){
    MountInfo list[MOUNT_SNAPSHOT_MAX];
    uint64_t n;
    {
        MutexGuard g(vfs_mutex());
        n = mount_snapshot(list, MOUNT_SNAPSHOT_MAX);
    }
    CHECK(n >= 1);
    const char* t = list[0].fstype;
    uint64_t tl = strlen_(t);
    memcpy(type, t, tl + 1);
    uint64_t dl = strlen_(list[0].dev_name);
    memcpy(dev, list[0].dev_name, dl + 1);
}

void variant_of(char* out){
    File* f = open_ok(none, "/VARIANT");
    int64_t n = rd(f, out, 32);
    file_put(f);
    CHECK(n > 1);
    while(n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')){
        n--;
    }
    out[n] = 0;
}

void mount_checks(){
    // Optiuni si dispozitive invalide
    EQ(vfs_mount(none, "sda2", "/mnt", "ext4", MS_RDONLY, "foo"), -EINVAL);
    EQ(vfs_mount(none, "sda1", "/mnt", "ext4", MS_RDONLY, nullptr), -EINVAL);
    EQ(vfs_mount(none, "nu_exista", "/mnt", "ext4", MS_RDONLY, nullptr), -ENODEV);
    // Inca o montare a aceleiasi partitii (cache-ul de blocuri e comun, deci coerent)
    EQ(vfs_mount(none, "/dev/sda2", "/mnt", "ext4", 0, "ro"), 0);
    CHECK(text_is("/mnt/data/hello.txt", "hello from ext4\n"));
    CHECK(text_is("/mnt/data/link_dir/hello.txt", "hello from ext4\n"));
    EQ(vfs_mkdir(none, "/mnt/nou", 0755), -EROFS);
    EQ(vfs_umount(none, "/mnt"), 0);
    kprintf("[OK] test_ext4 montare (optiuni, dispozitive invalide, remontare)\n");
}

void ext4_task(void* arg){
    Job* j = (Job*)arg;
    crc_init();
    char type[16], dev[32];
    root_mount(type, dev);
    if(j->mode == 0){
        CHECK(streq(type, "ext4"));
        CHECK(dev[0] != 0);
        EQ(fs_umount_pseudo(), 0);              // /dev, /proc, /tmp, /run sunt directoare ext4 goale in imagine
        char variant[32];
        variant_of(variant);
        bool indexed = !streq(variant, "linear");
        check_readonly();
        check_data(variant, indexed);
        mount_checks();
        Path root = none;
        path[0] = 0;
        walk(root, 0, 0);
        kprintf("[OK] test_ext4 arbore (%u intrari listate)\n", listed);
    } else {
        // Radacina a ramas initrd-ul; sda2 nu se poate monta fara "noload" cat timp jurnalul cere recuperare
        CHECK(streq(type, "tarfs"));
        EQ(vfs_mount(none, "sda2", "/mnt", "ext4", MS_RDONLY, nullptr), -EINVAL);
        EQ(vfs_mount(none, "sda2", "/mnt", "ext4", MS_RDONLY, "ro"), -EINVAL);
        if(j->mode == 2){
            EQ(vfs_mount(none, "sda2", "/mnt", "ext4", MS_RDONLY, "noload"), 0);
            CHECK(text_is("/mnt/data/hello.txt", "hello from ext4\n"));
            Path mnt = none;
            EQ(vfs_chdir(mnt, "/mnt"), 0);
            path[0] = 0;
            walk(mnt, 0, 0);
            vfs_path_release(mnt);
            EQ(vfs_umount(none, "/mnt"), 0);
            kprintf("[OK] test_ext4 noload (%u intrari listate)\n", listed);
        }
        kprintf("[OK] test_ext4 refuz (montarea esueaza, radacina ramane initrd)\n");
    }
    j->done = true;
}

}

void test_ext4_run(const char* mode){
    Job job = {0, false};
    if(mode && memcmp(mode, "refuse", 6) == 0){
        job.mode = 1;
    } else if(mode && memcmp(mode, "recover", 7) == 0){
        job.mode = 2;
    }
    Task* t = task_create_kernel("t-ext4", ext4_task, &job);
    CHECK(t);
    sched_add(t);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 6000 && !job.done; i++){
        sleep_ms(50);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    kprintf("[OK] test_ext4 (%s)\n", job.mode == 0 ? "ext4 read-only: radacina, fisiere, directoare, symlink-uri" :
            "montare refuzata");
}
