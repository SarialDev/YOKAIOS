#include "tests/test_ext4w.h"
#include "fs/ext4/ext4.h"
#include "fs/jbd2/jbd2.h"
#include "fs/vfs/vfs.h"
#include "hal/block.h"
#include "block/blockdev.h"
#include "core/panic.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"
#include "io.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_ext4w:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_ext4w:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};
const uint32_t ISL_ID = 50;                     // /w/isl: insule de 4 KiB la k * 64 KiB, goluri intre ele

struct Job {
    volatile bool done;
};

char ctl[513];
uint8_t buf[65536];
uint8_t buf2[65536];
uint32_t crc_table[256];
uint32_t g_islands;                             // cate insule are /w/isl in starea asteptata
int g_crash = -1;

// ---- utilitare ----

uint64_t slen(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

bool streq(const char* a, const char* b){
    uint64_t n = slen(a);
    return n == slen(b) && memcmp(a, b, n) == 0;
}

// Valoarea cheii "key=" din sectorul de control (pana la spatiu), nullptr daca lipseste
const char* ctl_get(const char* key, char* out, uint64_t cap){
    uint64_t kl = slen(key);
    for(const char* p = ctl; *p; p++){
        if((p == ctl || p[-1] == ' ') && memcmp(p, key, kl) == 0){
            uint64_t n = 0;
            p += kl;
            while(p[n] && p[n] != ' ' && n + 1 < cap){
                out[n] = p[n];
                n++;
            }
            out[n] = 0;
            return out;
        }
    }
    return nullptr;
}

uint32_t ctl_num(const char* key, uint32_t def){
    char v[16];
    if(!ctl_get(key, v, sizeof(v))){
        return def;
    }
    uint32_t x = 0;
    for(const char* p = v; *p >= '0' && *p <= '9'; p++){
        x = x * 10 + (uint32_t)(*p - '0');
    }
    return x;
}

void ctl_load(){
    BlockDevice* dev = nullptr;
    for(int k = 0; k < hal_count(HalKind::Block); k++){
        if(streq(hal_name(HalKind::Block, k), "sda1")){
            dev = hal_block(k);
        }
    }
    CHECK(dev);
    EQ(block_read_bytes(dev, 0, ctl, 512), 0);
    ctl[512] = 0;
    for(int i = 0; i < 512; i++){
        if(ctl[i] == '\n' || ctl[i] == '\r'){
            ctl[i] = 0;
            break;
        }
    }
    CHECK(memcmp(ctl, "ext4w ", 6) == 0);
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

uint32_t crc_str(const char* s){
    return ~crc_update(0xFFFFFFFFu, (const uint8_t*)s, slen(s));
}

// Continutul fisierelor de test: primii 4 octeti = id-ul (little endian), restul generati din (id, offset)
uint8_t byte_at(uint32_t id, uint64_t off){
    if(off < 4){
        return (uint8_t)(id >> (8 * off));
    }
    return (uint8_t)(off * 167 + (off >> 11) * 13 + id * 59);
}

uint8_t expected(uint32_t id, uint64_t off){
    if(id == ISL_ID && ((off & 0xFFFF) >= 4096 || (off >> 16) >= g_islands)){
        return 0;
    }
    return byte_at(id, off);
}

uint8_t pat(uint64_t off){                      // continutul fisierelor din /data (tests/mkext4.sh)
    return (uint8_t)(off * 131 + (off >> 9));
}

void put_num(char* out, const char* prefix, uint32_t n, int digits){
    uint64_t k = slen(prefix);
    memcpy(out, prefix, k);
    for(int i = digits - 1; i >= 0; i--){
        out[k + i] = (char)('0' + n % 10);
        n /= 10;
    }
    out[k + digits] = 0;
}

File* open_ok(const char* p, int flags, uint32_t mode = 0644){
    File* f;
    int err = vfs_open(none, p, flags, mode, f);
    if(err){
        panic("test_ext4w: open %s = %d", p, err);
    }
    return f;
}

void seek(File* f, uint64_t pos){
    EQ(f->ops->lseek(f, (int64_t)pos, SEEK_SET), (int64_t)pos);
}

// Scrie [from, to) cu continutul lui id, in bucati de chunk octeti
void write_range(File* f, uint32_t id, uint64_t from, uint64_t to, uint32_t chunk){
    seek(f, from);
    for(uint64_t o = from; o < to;){
        uint64_t n = to - o < chunk ? to - o : chunk;
        for(uint64_t i = 0; i < n; i++){
            buf[i] = byte_at(id, o + i);
        }
        EQ(f->ops->write(f, buf, n, false), (int64_t)n);
        o += n;
    }
}

void write_new(const char* p, uint32_t id, uint64_t size, uint32_t chunk = 7000){
    File* f = open_ok(p, O_CREAT | O_EXCL | O_WRONLY);
    write_range(f, id, 0, size, chunk);
    file_put(f);
}

void append(const char* p, uint32_t id, uint64_t from, uint64_t to){
    File* f = open_ok(p, O_WRONLY);
    write_range(f, id, from, to, 3001);
    file_put(f);
}

void trunc(const char* p, uint64_t size){
    File* f = open_ok(p, O_WRONLY);
    EQ(f->inode->ops->truncate(f->inode, size), 0);
    file_put(f);
}

void info(Ext4Info& inf){
    File* f = open_ok("/mnt", O_RDONLY | O_DIRECTORY);
    CHECK(ext4_info(f->inode, inf));
    file_put(f);
}

void commit(){
    File* f = open_ok("/mnt", O_RDONLY | O_DIRECTORY);
    EQ(ext4_commit(f->inode), 0);
    file_put(f);
}

void must(int err, const char* what, const char* p){
    if(err){
        panic("test_ext4w: %s %s = %d", what, p, err);
    }
}

// CRC32 (zlib) si dimensiunea unui fisier, citit in bucati de chunk
uint32_t file_crc(const char* p, uint64_t& size, uint32_t chunk = 65536){
    File* f = open_ok(p, O_RDONLY);
    uint32_t crc = 0xFFFFFFFFu;
    size = 0;
    int64_t r;
    while((r = f->ops->read(f, buf2, chunk, false)) > 0){
        crc = crc_update(crc, buf2, (uint64_t)r);
        size += (uint64_t)r;
    }
    EQ(r, 0);
    file_put(f);
    return ~crc;
}

// Continutul e exact expected(id, ...) pe size octeti
void check_content(const char* p, uint32_t id, uint64_t size){
    File* f = open_ok(p, O_RDONLY);
    uint64_t off = 0;
    int64_t r;
    static const uint32_t chunks[] = {65536, 4096, 10007, 512};
    uint32_t k = 0;
    while((r = f->ops->read(f, buf2, chunks[k++ % 4], false)) > 0){
        for(int64_t i = 0; i < r; i++){
            if(buf2[i] != expected(id, off + (uint64_t)i)){
                panic("test_ext4w: %s: octet gresit la %lu (%x, asteptat %x)", p, off + (uint64_t)i, buf2[i],
                      expected(id, off + (uint64_t)i));
            }
        }
        off += (uint64_t)r;
    }
    EQ(r, 0);
    EQ(off, size);
    file_put(f);
}

// Parcurge directorul p (cale absoluta): fn(cale copil, stat) pentru fiecare intrare, fara . si ..
template<typename F>
uint32_t for_each(const char* p, F fn){
    File* d = open_ok(p, O_RDONLY | O_DIRECTORY);
    uint8_t dbuf[2048];
    int64_t got;
    uint32_t n = 0;
    char child[256];
    uint64_t pl = slen(p);
    while((got = d->ops->getdents(d, dbuf, sizeof(dbuf))) > 0){
        for(int64_t off = 0; off < got;){
            Dirent64* e = (Dirent64*)(dbuf + off);
            off += e->d_reclen;
            const char* name = e->d_name;
            if(name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))){
                continue;
            }
            uint64_t nl = slen(name);
            CHECK(pl + 1 + nl < sizeof(child));
            memcpy(child, p, pl);
            child[pl] = '/';
            memcpy(child + pl + 1, name, nl + 1);
            Stat st;
            EQ(vfs_stat(none, child, false, st), 0);
            n++;
            fn(child, name, st);
        }
    }
    EQ(got, 0);
    file_put(d);
    return n;
}

// ---- liniile pentru gazda ----

struct DirSum {
    uint32_t count;
    uint32_t sum;
    uint32_t xr;
};

DirSum dir_sum(const char* p){
    DirSum s = {0, 0, 0};
    for_each(p, [&](const char*, const char* name, const Stat&){
        uint32_t c = crc_str(name);
        s.count++;
        s.sum += c;
        s.xr ^= c;
    });
    return s;
}

void print_tree(const char* tag, const char* p, int depth = 0){
    CHECK(depth < 16);
    uint32_t n = for_each(p, [&](const char* c, const char*, const Stat& st){
        if(S_ISDIR(st.st_mode)){
            print_tree(tag, c, depth + 1);
        } else if(S_ISREG(st.st_mode)){
            uint64_t size;
            uint32_t crc = file_crc(c, size);
            EQ(size, st.st_size);
            kprintf("%s F %08x %lu %s\n", tag, crc, size, c + 4);
        } else if(S_ISLNK(st.st_mode)){
            char tgt[300];
            int64_t r = vfs_readlink(none, c, tgt, sizeof(tgt) - 1);
            CHECK(r >= 0);
            tgt[r] = 0;
            kprintf("%s L %s -> %s\n", tag, c + 4, tgt);
        } else {
            panic("test_ext4w: tip neasteptat %o la %s", st.st_mode, c);
        }
    });
    kprintf("%s D %s %u\n", tag, p[4] ? p + 4 : "/", n);
}

// Liniile tag pentru radacinile date (cai absolute sub /mnt); directoarele "M" doar cu suma numelor
void print_lines(const char* tag, const char* const* roots, const char* const* big){
    for(; *roots; roots++){
        Stat st;
        int err = vfs_stat(none, *roots, false, st);
        if(err == -ENOENT){
            kprintf("%s A %s\n", tag, *roots + 4);
        } else if(S_ISDIR(st.st_mode)){
            print_tree(tag, *roots);
        } else {
            uint64_t size;
            uint32_t crc = file_crc(*roots, size);
            kprintf("%s F %08x %lu %s\n", tag, crc, size, *roots + 4);
        }
    }
    for(; big && *big; big++){
        DirSum s = dir_sum(*big);
        kprintf("%s M %s %u %08x %08x\n", tag, *big + 4, s.count, s.sum, s.xr);
    }
}

// ---- modelul starilor S1 / S2 ----

enum : uint8_t { T_DIR, T_FILE, T_LINK };

struct Ent {
    char path[48];
    uint8_t type;
    bool seen;
    uint32_t id;
    uint64_t size;
    const char* target;
};

Ent model[320];
uint32_t nmodel;

const uint64_t SIZES[12] = {0, 1, 100, 4095, 4096, 4097, 20000, 100000, 300000, 1000000, 8191, 12289};
char long_target[130];

Ent& add(const char* p, uint8_t type){
    CHECK(nmodel < 320);
    Ent& e = model[nmodel++];
    uint64_t n = slen(p);
    CHECK(n < sizeof(e.path));
    memcpy(e.path, p, n + 1);
    e.type = type;
    e.seen = false;
    e.id = 0;
    e.size = 0;
    e.target = nullptr;
    return e;
}

void add_file(const char* p, uint32_t id, uint64_t size){
    Ent& e = add(p, T_FILE);
    e.id = id;
    e.size = size;
}

void add_link(const char* p, const char* tgt){
    add(p, T_LINK).target = tgt;
}

void lin_name(char* out, char c, uint32_t i){
    char pre[16] = "/mnt/w/lin/x";
    pre[11] = c;
    put_num(out, pre, i, 3);
}

void build_model(int state){
    nmodel = 0;
    char p[48];
    add("/mnt/w", T_DIR);
    add("/mnt/w/d1", T_DIR);
    add("/mnt/w/lin", T_DIR);
    add_link("/mnt/w/d1/sl_long", long_target);
    for(uint32_t i = 0; i < 200; i++){
        if(state == 2 && i % 4 == 0){
            continue;
        }
        lin_name(p, 'e', i);
        add_file(p, 1000 + i, (i % 5) * 700);
    }
    if(state == 1){
        add("/mnt/w/d1/d2", T_DIR);
        for(uint32_t i = 0; i < 12; i++){
            put_num(p, "/mnt/w/f", i, 2);
            add_file(p, i + 1, SIZES[i]);
        }
        add_link("/mnt/w/sl_short", "f01");
        add_file("/mnt/w/isl", ISL_ID, 399ull * 65536 + 4096);
        g_islands = 400;
        return;
    }
    static const uint32_t keep[] = {0, 1, 2, 10, 11};
    for(uint32_t i : keep){
        put_num(p, "/mnt/w/f", i, 2);
        add_file(p, i + 1, SIZES[i]);
    }
    add_file("/mnt/w/f05", 6, 4097 + 10000);
    add_file("/mnt/w/f07", 7, SIZES[6]);                // f06 redenumit peste f07
    add_file("/mnt/w/f08", 9, 70000);
    add_file("/mnt/w/f09", 10, 0);
    add_file("/mnt/w/d1/f04r", 5, SIZES[4]);
    add_file("/mnt/w/s2new", 60, 50000);
    add("/mnt/w/s2dir", T_DIR);
    add_file("/mnt/w/s2dir/a", 61, 5000);
    add_file("/mnt/w/s2dir/b", 62, 0);
    for(uint32_t i = 0; i < 20; i++){
        lin_name(p, 'x', i);
        add_file(p, 2000 + i, 0);
    }
    add_link("/mnt/w/sl2", "s2new");
    add_file("/mnt/w/isl", ISL_ID, 100ull * 65536);
    g_islands = 100;
}

Ent* find(const char* p){
    for(uint32_t i = 0; i < nmodel; i++){
        if(streq(model[i].path, p)){
            return &model[i];
        }
    }
    return nullptr;
}

void verify_tree(const char* p, int depth = 0){
    CHECK(depth < 8);
    for_each(p, [&](const char* c, const char*, const Stat& st){
        Ent* e = find(c);
        if(!e){
            panic("test_ext4w: intrare in plus %s", c);
        }
        CHECK(!e->seen);
        e->seen = true;
        if(e->type == T_DIR){
            CHECK(S_ISDIR(st.st_mode));
            verify_tree(c, depth + 1);
        } else if(e->type == T_FILE){
            CHECK(S_ISREG(st.st_mode));
            if((uint64_t)st.st_size != e->size){
                panic("test_ext4w: %s are %lu octeti, asteptat %lu", c, (uint64_t)st.st_size, e->size);
            }
            check_content(c, e->id, e->size);
        } else {
            CHECK(S_ISLNK(st.st_mode));
            char tgt[300];
            int64_t r = vfs_readlink(none, c, tgt, sizeof(tgt));
            EQ(r, slen(e->target));
            CHECK(memcmp(tgt, e->target, (uint64_t)r) == 0);
        }
    });
}

// Numele din /many (3000 de fisiere f%05d din imagine): S1 sterge f(7i), i < 100, si adauga n00000..n00199;
// S2 sterge n00000..n00049 si adauga m00000..m00029
DirSum many_expected(int state){
    DirSum s = {0, 0, 0};
    char p[16];
    auto addn = [&](const char* pre, uint32_t i){
        put_num(p, pre, i, 5);
        uint32_t c = crc_str(p);
        s.count++;
        s.sum += c;
        s.xr ^= c;
    };
    for(uint32_t i = 0; i < 3000; i++){
        if(i % 7 || i / 7 >= 100){
            addn("f", i);
        }
    }
    for(uint32_t i = state == 2 ? 50 : 0; i < 200; i++){
        addn("n", i);
    }
    for(uint32_t i = 0; state == 2 && i < 30; i++){
        addn("m", i);
    }
    return s;
}

void verify_state(int state){
    build_model(state);
    Stat st;
    EQ(vfs_stat(none, "/mnt/w", false, st), 0);
    model[0].seen = true;
    verify_tree("/mnt/w");
    for(uint32_t i = 0; i < nmodel; i++){
        if(!model[i].seen){
            panic("test_ext4w: lipseste %s (starea S%d)", model[i].path, state);
        }
    }
    DirSum got = dir_sum("/mnt/many");
    DirSum want = many_expected(state);
    EQ(got.count, want.count);
    EQ(got.sum, want.sum);
    EQ(got.xr, want.xr);
    EQ(vfs_stat(none, "/mnt/many/f00007", false, st), -ENOENT);
    EQ(vfs_stat(none, "/mnt/many/f00008", false, st), 0);
    EQ(vfs_stat(none, "/mnt/many/n00010", false, st), state == 2 ? -ENOENT : 0);
    EQ(vfs_stat(none, "/mnt/many/m00005", false, st), state == 2 ? 0 : -ENOENT);
    EQ(vfs_stat(none, "/mnt/data/big.bin", false, st), -ENOENT);
    EQ(vfs_stat(none, "/mnt/data/sizes/size_1000000", false, st), 0);
    EQ(st.st_size, 300000);
    File* f = open_ok("/mnt/data/sizes/size_1000000", O_RDONLY);
    for(uint64_t off = 0; off < 300000;){
        int64_t r = f->ops->read(f, buf2, 50000, false);
        CHECK(r > 0);
        for(int64_t i = 0; i < r; i++){
            CHECK(buf2[i] == pat(off + (uint64_t)i));
        }
        off += (uint64_t)r;
    }
    file_put(f);
    kprintf("[OK] ext4w starea S%d verificata (%u intrari in /w, %u in /many)\n", state, nmodel, got.count);
}

void apply_s1(){
    must(vfs_mkdir(none, "/mnt/w", 0755), "mkdir", "/mnt/w");
    must(vfs_mkdir(none, "/mnt/w/d1", 0755), "mkdir", "/mnt/w/d1");
    must(vfs_mkdir(none, "/mnt/w/d1/d2", 0755), "mkdir", "/mnt/w/d1/d2");
    must(vfs_mkdir(none, "/mnt/w/lin", 0755), "mkdir", "/mnt/w/lin");
    char p[48];
    static const uint32_t chunks[] = {7000, 4096, 65536, 333};
    for(uint32_t i = 0; i < 12; i++){
        put_num(p, "/mnt/w/f", i, 2);
        write_new(p, i + 1, SIZES[i], chunks[i % 4]);
    }
    {
        // 400 de insule: arbore de extente cu index (radacina -> 2 frunze)
        File* f = open_ok("/mnt/w/isl", O_CREAT | O_EXCL | O_WRONLY);
        for(uint64_t k = 0; k < 400; k++){
            write_range(f, ISL_ID, k * 65536, k * 65536 + 4096, 4096);
        }
        file_put(f);
    }
    for(uint32_t i = 0; i < 200; i++){
        lin_name(p, 'e', i);
        write_new(p, 1000 + i, (i % 5) * 700);
    }
    must(vfs_symlink(none, "f01", "/mnt/w/sl_short"), "symlink", "sl_short");
    must(vfs_symlink(none, long_target, "/mnt/w/d1/sl_long"), "symlink", "sl_long");
    for(uint32_t i = 0; i < 200; i++){
        put_num(p, "/mnt/many/n", i, 5);
        write_new(p, 0, 0);
    }
    for(uint32_t i = 0; i < 100; i++){
        put_num(p, "/mnt/many/f", i * 7, 5);
        must(vfs_unlink(none, p), "unlink", p);
    }
    must(vfs_unlink(none, "/mnt/data/big.bin"), "unlink", "big.bin");
    trunc("/mnt/data/sizes/size_1000000", 300000);
    commit();
}

void apply_s2(){
    char p[48];
    write_new("/mnt/w/s2new", 60, 50000);
    append("/mnt/w/f05", 6, 4097, 4097 + 10000);
    trunc("/mnt/w/f08", 70000);
    trunc("/mnt/w/f09", 0);
    trunc("/mnt/w/isl", 100ull * 65536);
    must(vfs_unlink(none, "/mnt/w/f03"), "unlink", "f03");
    must(vfs_rmdir(none, "/mnt/w/d1/d2"), "rmdir", "d2");
    must(vfs_rename(none, "/mnt/w/f04", "/mnt/w/d1/f04r"), "rename", "f04");
    must(vfs_rename(none, "/mnt/w/f06", "/mnt/w/f07"), "rename", "f06");
    must(vfs_mkdir(none, "/mnt/w/s2dir", 0755), "mkdir", "s2dir");
    write_new("/mnt/w/s2dir/a", 61, 5000);
    write_new("/mnt/w/s2dir/b", 62, 0);
    for(uint32_t i = 0; i < 200; i += 4){
        lin_name(p, 'e', i);
        must(vfs_unlink(none, p), "unlink", p);
    }
    for(uint32_t i = 0; i < 20; i++){
        lin_name(p, 'x', i);
        write_new(p, 2000 + i, 0);
    }
    for(uint32_t i = 0; i < 50; i++){
        put_num(p, "/mnt/many/n", i, 5);
        must(vfs_unlink(none, p), "unlink", p);
    }
    for(uint32_t i = 0; i < 30; i++){
        put_num(p, "/mnt/many/m", i, 5);
        write_new(p, 0, 0);
    }
    must(vfs_unlink(none, "/mnt/w/sl_short"), "unlink", "sl_short");
    must(vfs_symlink(none, "s2new", "/mnt/w/sl2"), "symlink", "sl2");
}

void crash_hook(int stage){
    if(stage == g_crash){
        kprintf("[OK] ext4w cadere la etapa %d\n", stage);
        outb(0xF4, 0x10);               // QEMU se opreste pe loc (isa-debug-exit, codul 33)
        for(;;){
            __asm__ volatile("cli; hlt");
        }
    }
}

void mount_rw(const char* opts){
    int err = vfs_mount(none, "sda2", "/mnt", "ext4", 0, opts);
    if(err){
        panic("test_ext4w: montarea sda2 (%s) = %d", opts, err);
    }
    Ext4Info inf;
    info(inf);
    CHECK(inf.rw);
    kprintf("ext4w: montat rw, jurnal reluat: %lu tranzactii, %lu blocuri, %lu sarite (revoke)%s\n", inf.replayed_txns,
            inf.replayed_blocks, inf.replay_revoked, inf.journal_csum ? ", checksum v3" : "");
}

void umount(){
    EQ(vfs_umount(none, "/mnt"), 0);
}

const char* const W_ROOTS[] = {"/mnt/w", "/mnt/data/sizes/size_1000000", "/mnt/data/big.bin", nullptr};
const char* const W_BIG[] = {"/mnt/many", nullptr};

void mode_write(){
    g_crash = (int)ctl_num("crash=", 0);
    mount_rw("rw,commit=3600");
    apply_s1();
    verify_state(1);
    Ext4Info a;
    info(a);
    kprintf("[OK] ext4w S1 comis (%lu tranzactii, %lu blocuri jurnalizate, %lu revoke)\n", a.commits,
            a.journaled_blocks, a.revokes);
    apply_s2();
    Ext4Info b;
    info(b);
    EQ(b.commits, a.commits);                   // S2 e o singura tranzactie: nimic comis inca
    CHECK(b.pending > 0);
    kprintf("[OK] ext4w S2 pregatita (%u blocuri in tranzactie)\n", b.pending);
    if(g_crash == 5){
        kprintf("[OK] ext4w cadere la etapa 5\n");
        outb(0xF4, 0x10);
        for(;;){
            __asm__ volatile("cli; hlt");
        }
    }
    jbd2_crash_hook = g_crash ? crash_hook : nullptr;
    commit();
    jbd2_crash_hook = nullptr;
    CHECK(g_crash == 0);
    verify_state(2);
    umount();
    kprintf("[OK] ext4w umount curat\n");
}

void mode_verify(){
    g_crash = (int)ctl_num("crash=", 0);
    int state = (g_crash == 1 || g_crash == 5) ? 1 : 2;
    uint64_t want_txns = (g_crash >= 2 && g_crash <= 4) ? 1 : 0;
    mount_rw("rw");
    Ext4Info inf;
    info(inf);
    EQ(inf.replayed_txns, want_txns);
    kprintf("[OK] ext4w replay (%lu tranzactii asteptate)\n", want_txns);
    verify_state(state);
    print_lines("EXT4S", W_ROOTS, W_BIG);
    // Sistemul de fisiere reluat se poate folosi mai departe
    write_new("/mnt/w/boot2", 70, 200000);
    must(vfs_unlink(none, "/mnt/w/f00"), "unlink", "f00");
    must(vfs_mkdir(none, "/mnt/w/boot2dir", 0755), "mkdir", "boot2dir");
    must(vfs_rename(none, "/mnt/w/f01", "/mnt/w/boot2dir/f01"), "rename", "f01");
    trunc("/mnt/w/f02", 7);
    commit();
    check_content("/mnt/w/boot2", 70, 200000);
    print_lines("EXT4W", W_ROOTS, W_BIG);
    umount();
    kprintf("[OK] ext4w umount curat\n");
}

// ---- stres: scrieri la intamplare pana la SIGKILL ----

uint32_t rng;
uint32_t rnd(){
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void st_name(char* p){
    uint32_t d = rnd() % 5;
    if(d == 4){
        put_num(p, "/mnt/st/f", rnd() % 20, 2);
    } else {
        char pre[24] = "/mnt/st/dX/f";
        pre[9] = (char)('0' + d);
        put_num(p, pre, rnd() % 20, 2);
    }
}

bool soft(int err){
    return err == 0 || err == -ENOENT || err == -EEXIST || err == -ENOTEMPTY || err == -ENOSPC || err == -EISDIR ||
           err == -ENOTDIR;
}

void soft_ok(int err, const char* what, const char* p){
    if(!soft(err)){
        panic("test_ext4w: stres: %s %s = %d", what, p, err);
    }
}

uint32_t st_ctr;

// Scrie [from, to) cu id; o eroare ENOSPC opreste scrierea (continutul scris ramane consistent)
void st_write(File* f, uint32_t id, uint64_t from, uint64_t to){
    seek(f, from);
    for(uint64_t o = from; o < to;){
        uint64_t n = rnd() % 20000 + 4;
        if(n > to - o){
            n = to - o;
        }
        for(uint64_t i = 0; i < n; i++){
            buf[i] = byte_at(id, o + i);
        }
        int64_t w = f->ops->write(f, buf, n, false);
        if(w == -ENOSPC){
            return;
        }
        EQ(w, (int64_t)n);
        o += n;
    }
}

void stress_op(){
    char p[48], q[48];
    st_name(p);
    uint32_t r = rnd() % 100;
    Ext4Info inf;
    info(inf);
    if(inf.free_blocks < 3000){
        r = 50;                                 // aproape plin: doar stergeri
    }
    if(r < 30){
        File* f;
        int err = vfs_open(none, p, O_CREAT | O_EXCL | O_WRONLY, 0644, f);
        soft_ok(err, "create", p);
        if(!err){
            uint32_t id = ++st_ctr;
            uint64_t size = rnd() % 8 == 0 ? 400000 + rnd() % 300000 : 4 + rnd() % 60000;
            st_write(f, id, 0, size);
            file_put(f);
        }
    } else if(r < 50){
        File* f;
        int err = vfs_open(none, p, O_RDWR, 0, f);
        soft_ok(err, "open", p);
        if(!err){
            uint64_t size = f->inode->size;
            uint32_t id = ++st_ctr;
            if(size >= 4){
                seek(f, 0);
                uint8_t h[4];
                EQ(f->ops->read(f, h, 4, false), 4);
                id = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
                st_write(f, id, size, size + 1 + rnd() % 30000);
            } else if(size == 0){
                st_write(f, id, 0, 4 + rnd() % 30000);
            }
            file_put(f);
        }
    } else if(r < 62){
        soft_ok(vfs_unlink(none, p), "unlink", p);
    } else if(r < 74){
        File* f;
        int err = vfs_open(none, p, O_WRONLY, 0, f);
        soft_ok(err, "open", p);
        if(!err){
            uint64_t size = f->inode->size;
            uint64_t ns = size > 4 && rnd() % 4 ? 4 + rnd() % (size - 4) : 0;
            int e2 = f->inode->ops->truncate(f->inode, ns);
            soft_ok(e2, "truncate", p);
            file_put(f);
        }
    } else if(r < 84){
        st_name(q);
        int err = vfs_rename(none, p, q);
        soft_ok(err == -EINVAL ? 0 : err, "rename", p);
    } else if(r < 90){
        char d[24] = "/mnt/st/dX";
        d[9] = (char)('0' + rnd() % 4);
        soft_ok(rnd() % 2 ? vfs_mkdir(none, d, 0755) : vfs_rmdir(none, d), "mkdir/rmdir", d);
    } else if(r < 96){
        if(rnd() % 2){
            char t[200];
            uint32_t n = rnd() % 2 ? 6 : 61 + rnd() % 120;
            for(uint32_t i = 0; i < n; i++){
                t[i] = (char)('a' + i % 26);
            }
            t[n] = 0;
            soft_ok(vfs_symlink(none, t, p), "symlink", p);
        } else {
            soft_ok(vfs_unlink(none, p), "unlink", p);
        }
    } else {
        commit();
    }
}

void mode_stress(){
    rng = ctl_num("seed=", 1) * 2654435761u + 1;
    mount_rw("rw,commit=1");
    must(vfs_mkdir(none, "/mnt/st", 0755), "mkdir", "/mnt/st");
    for(int d = 0; d < 4; d++){
        char p[24] = "/mnt/st/dX";
        p[9] = (char)('0' + d);
        must(vfs_mkdir(none, p, 0755), "mkdir", p);
    }
    commit();
    kprintf("[OK] ext4w stres pornit\n");
    for(uint64_t n = 1;; n++){
        stress_op();
        if(n % 200 == 0){
            Ext4Info inf;
            info(inf);
            kprintf("ext4w stres: %lu operatii, %lu commit-uri, %lu blocuri libere\n", n, inf.commits,
                    inf.free_blocks);
        }
    }
}

uint32_t sc_files, sc_links;
uint64_t sc_bytes;

void stress_check_dir(const char* p, int depth){
    CHECK(depth < 4);
    for_each(p, [&](const char* c, const char*, const Stat& st){
        if(S_ISDIR(st.st_mode)){
            stress_check_dir(c, depth + 1);
        } else if(S_ISREG(st.st_mode)){
            sc_files++;
            sc_bytes += (uint64_t)st.st_size;
            if(st.st_size >= 4){
                File* f = open_ok(c, O_RDONLY);
                uint8_t h[4];
                EQ(f->ops->read(f, h, 4, false), 4);
                file_put(f);
                g_islands = 0;
                check_content(c, (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24,
                              (uint64_t)st.st_size);
            }
        } else if(S_ISLNK(st.st_mode)){
            sc_links++;
            char t[300];
            int64_t r = vfs_readlink(none, c, t, sizeof(t));
            EQ(r, st.st_size);
            for(int64_t i = 0; i < r; i++){
                CHECK(t[i] == (char)('a' + i % 26));
            }
        } else {
            panic("test_ext4w: tip neasteptat in %s", c);
        }
    });
}

void mode_stresscheck(){
    mount_rw("rw");
    stress_check_dir("/mnt/st", 0);
    kprintf("[OK] ext4w stres verificat (%u fisiere consistente, %lu octeti, %u symlink-uri)\n", sc_files, sc_bytes,
            sc_links);
    static const char* const roots[] = {"/mnt/st", nullptr};
    print_lines("EXT4S", roots, nullptr);
    umount();
    kprintf("[OK] ext4w umount curat\n");
}

// ---- jurnal scris de debugfs ----

const char* const R_ROOTS[] = {"/mnt/w2", "/mnt/data/hello.txt", nullptr};

void mode_replay(){
    bool csum = ctl_num("csum=", 0) != 0;
    mount_rw("rw,commit=3600");
    Ext4Info inf;
    info(inf);
    // tranzactia 1: blocurile 0 si 1 din big.bin = 0xA5; tranzactia 2: blocul 0 = 0x5A, revoke pentru blocul 1
    EQ(inf.replayed_txns, 2);
    EQ(inf.replayed_blocks, 2);
    EQ(inf.replay_revoked, 1);
    CHECK(inf.journal_csum == csum);
    File* f = open_ok("/mnt/data/big.bin", O_RDONLY);
    EQ(f->ops->read(f, buf2, 3 * 4096, false), 3 * 4096);
    file_put(f);
    for(uint32_t i = 0; i < 4096; i++){
        CHECK(buf2[i] == 0x5A);
    }
    for(uint32_t i = 4096; i < 3 * 4096; i++){
        CHECK(buf2[i] == pat(i));
    }
    kprintf("[OK] ext4w replay debugfs (2 tranzactii, revoke respectat%s)\n", csum ? ", checksum v3" : "");
    // Scrieri peste jurnalul altcuiva, apoi cadere dupa commit: boot-ul urmator (si e2fsck pe gazda) le reiau
    must(vfs_mkdir(none, "/mnt/w2", 0755), "mkdir", "/mnt/w2");
    write_new("/mnt/w2/f", 80, 30000);
    must(vfs_symlink(none, long_target, "/mnt/w2/sl"), "symlink", "/mnt/w2/sl");
    must(vfs_mkdir(none, "/mnt/w2/sub", 0755), "mkdir", "/mnt/w2/sub");
    must(vfs_unlink(none, "/mnt/data/hello.txt"), "unlink", "hello.txt");
    g_crash = 2;
    jbd2_crash_hook = crash_hook;
    commit();
    panic("test_ext4w: commit-ul nu a ajuns la etapa 2");
}

void mode_replay2(){
    bool csum = ctl_num("csum=", 0) != 0;
    mount_rw("rw");
    Ext4Info inf;
    info(inf);
    EQ(inf.replayed_txns, 1);
    CHECK(inf.journal_csum == csum);
    check_content("/mnt/w2/f", 80, 30000);
    char t[300];
    EQ(vfs_readlink(none, "/mnt/w2/sl", t, sizeof(t)), slen(long_target));
    Stat st;
    EQ(vfs_stat(none, "/mnt/data/hello.txt", false, st), -ENOENT);
    EQ(vfs_stat(none, "/mnt/w2/sub", false, st), 0);
    kprintf("[OK] ext4w replay2 (jurnalul nostru%s reluat)\n", csum ? " cu checksum v3" : "");
    print_lines("EXT4W", R_ROOTS, nullptr);
    umount();
    kprintf("[OK] ext4w umount curat\n");
}

void ext4w_task(void* arg){
    Job* j = (Job*)arg;
    crc_init();
    for(int i = 0; i < 120; i++){
        long_target[i] = (char)('A' + i % 26);
    }
    long_target[120] = 0;
    ctl_load();
    kprintf("ext4w: control \"%s\"\n", ctl);
    char mode[16];
    CHECK(ctl_get("mode=", mode, sizeof(mode)));
    if(streq(mode, "write")){
        mode_write();
    } else if(streq(mode, "verify")){
        mode_verify();
    } else if(streq(mode, "stress")){
        mode_stress();
    } else if(streq(mode, "stresscheck")){
        mode_stresscheck();
    } else if(streq(mode, "replay")){
        mode_replay();
    } else if(streq(mode, "replay2")){
        mode_replay2();
    } else {
        panic("test_ext4w: mod necunoscut %s", mode);
    }
    j->done = true;
}

}

void test_ext4w_run(){
    Job job = {false};
    Task* t = task_create_kernel("t-ext4w", ext4w_task, &job);
    CHECK(t);
    sched_add(t);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 12000 && !job.done; i++){
        sleep_ms(50);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    kprintf("[OK] test_ext4w\n");
}
