#include "tests/test_fs.h"
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
        if(got_ != want_){ panic("test_fs:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_fs:%d: %s", __LINE__, #cond); } } while(0)

const Path none = {};

struct Job {
    volatile bool done;
};

struct Fs {
    const char* name;       // "fat32" / "ext4"
    const char* root;       // punctul de montare
    uint32_t idx;           // 0 / 1: intra in semintele continutului nou
};

const Fs FS[2] = {{"fat32", "/media/disk", 0}, {"ext4", "/mnt", 1}};

uint8_t buf[65536];

// ---- SHA-256 (FIPS 180-4) ----

struct Sha256 {
    uint32_t h[8];
    uint8_t blk[64];
    uint64_t total;
    uint32_t fill;
};

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t ror(uint32_t x, int n){
    return (x >> n) | (x << (32 - n));
}

void sha_block(Sha256& s, const uint8_t* p){
    uint32_t w[64];
    for(int i = 0; i < 16; i++){
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for(int i = 16; i < 64; i++){
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for(int i = 0; i < 64; i++){
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}

void sha_init(Sha256& s){
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for(int i = 0; i < 8; i++){
        s.h[i] = iv[i];
    }
    s.total = 0;
    s.fill = 0;
}

void sha_update(Sha256& s, const uint8_t* p, uint64_t n){
    s.total += n;
    while(n){
        uint64_t k = 64 - s.fill;
        if(k > n){
            k = n;
        }
        memcpy(s.blk + s.fill, p, k);
        s.fill += (uint32_t)k;
        p += k;
        n -= k;
        if(s.fill == 64){
            sha_block(s, s.blk);
            s.fill = 0;
        }
    }
}

void sha_final(Sha256& s, char hex[65]){
    uint64_t bits = s.total * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while(s.fill != 56){
        sha_update(s, &pad, 1);
    }
    uint8_t len[8];
    for(int i = 0; i < 8; i++){
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha_update(s, len, 8);
    static const char dig[] = "0123456789abcdef";
    for(int i = 0; i < 32; i++){
        uint8_t b = (uint8_t)(s.h[i / 4] >> (24 - 8 * (i % 4)));
        hex[2 * i] = dig[b >> 4];
        hex[2 * i + 1] = dig[b & 15];
    }
    hex[64] = 0;
}

// Vectori cunoscuti: "" si "abc"
void sha_selftest(){
    Sha256 s;
    char hex[65];
    sha_init(s);
    sha_final(s, hex);
    CHECK(memcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 64) == 0);
    sha_init(s);
    sha_update(s, (const uint8_t*)"abc", 3);
    sha_final(s, hex);
    CHECK(memcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 64) == 0);
}

// ---- utilitare ----

uint64_t slen(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

// Continutul fisierelor noi: octet (seed, offset), acelasi generator in tests/check_fs.sh
uint8_t gen_byte(uint32_t seed, uint64_t off){
    uint32_t x = (uint32_t)off * 0x9E3779B1u + seed * 0x85EBCA6Bu;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return (uint8_t)(x >> 8);
}

void path_join(char* out, uint64_t cap, const char* a, const char* b){
    uint64_t la = slen(a), lb = slen(b);
    CHECK(la + 1 + lb + 1 <= cap);
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
}

File* open_ok(const char* p, int flags){
    File* f;
    int err = vfs_open(none, p, flags, 0644, f);
    if(err){
        panic("test_fs: open %s = %d", p, err);
    }
    return f;
}

void wr(File* f, const uint8_t* p, uint64_t n){
    EQ(f->ops->write(f, p, n, false), (int64_t)n);
}

// Scrie un fisier nou de size octeti cu continutul generator(seed), in bucati de chunk octeti
void write_gen(const char* p, uint32_t seed, uint64_t size, uint32_t chunk){
    File* f = open_ok(p, O_CREAT | O_EXCL | O_WRONLY);
    for(uint64_t o = 0; o < size;){
        uint64_t n = size - o < chunk ? size - o : chunk;
        for(uint64_t i = 0; i < n; i++){
            buf[i] = gen_byte(seed, o + i);
        }
        wr(f, buf, n);
        o += n;
    }
    file_put(f);
}

// SHA-256 si dimensiunea unui fisier, citit in bucati de 64 KiB (sau chunk)
void file_sha(const char* p, uint64_t& size, char hex[65], uint32_t chunk = sizeof(buf)){
    File* f = open_ok(p, O_RDONLY);
    Sha256 s;
    sha_init(s);
    size = 0;
    int64_t r;
    while((r = f->ops->read(f, buf, chunk, false)) > 0){
        sha_update(s, buf, (uint64_t)r);
        size += (uint64_t)r;
    }
    EQ(r, 0);
    file_put(f);
    sha_final(s, hex);
}

// ---- manifeste ----

char man[8192];

// Citeste manifestul <root>/<name> in man; false daca nu exista
bool man_load(const Fs& fs, const char* name){
    char p[128];
    path_join(p, sizeof(p), fs.root, name);
    File* f;
    int err = vfs_open(none, p, O_RDONLY, 0, f);
    if(err == -ENOENT){
        return false;
    }
    EQ(err, 0);
    uint64_t n = 0;
    int64_t r;
    while((r = f->ops->read(f, (uint8_t*)man + n, sizeof(man) - 1 - n, false)) > 0){
        n += (uint64_t)r;
    }
    EQ(r, 0);
    file_put(f);
    man[n] = 0;
    return true;
}

uint64_t parse_num(const char*& p){
    uint64_t x = 0;
    CHECK(*p >= '0' && *p <= '9');
    while(*p >= '0' && *p <= '9'){
        x = x * 10 + (uint64_t)(*p++ - '0');
    }
    return x;
}

// Fiecare linie "<sha256> <dimensiune> <cale>" din man: fisierul trebuie sa existe cu exact acea dimensiune si acel
// SHA-256. Se citeste in chunk diferit la fiecare fisier ca sa treaca prin mai multe forme de citire
uint32_t man_verify(const Fs& fs){
    static const uint32_t chunks[] = {65536, 4096, 10007, 512, 1};
    uint32_t files = 0;
    uint64_t bytes = 0;
    for(const char* p = man; *p;){
        char want[65];
        CHECK(slen(p) >= 64);
        memcpy(want, p, 64);
        want[64] = 0;
        p += 64;
        CHECK(*p++ == ' ');
        uint64_t size = parse_num(p);
        CHECK(*p++ == ' ');
        char rel[96];
        uint32_t n = 0;
        while(*p && *p != '\n'){
            CHECK(n + 1 < sizeof(rel));
            rel[n++] = *p++;
        }
        rel[n] = 0;
        if(*p == '\n'){
            p++;
        }
        char path[128];
        path_join(path, sizeof(path), fs.root, rel);
        char got[65];
        uint64_t gsize;
        uint32_t chunk = chunks[files % 5];
        if(chunk == 1 && size > 20000){
            chunk = 3001;                       // citirea octet cu octet doar pe fisiere mici
        }
        file_sha(path, gsize, got, chunk);
        if(gsize != size || memcmp(got, want, 65) != 0){
            panic("test_fs: %s %s: dimensiune %lu (asteptat %lu), sha256 %s (asteptat %s)", fs.name, rel, gsize, size,
                  got, want);
        }
        files++;
        bytes += size;
    }
    kprintf("test_fs: %s: %u fisiere, %lu octeti verificati (SHA-256)\n", fs.name, files, bytes);
    return files;
}

void must_missing(const Fs& fs, const char* rel){
    char p[128];
    path_join(p, sizeof(p), fs.root, rel);
    Stat st;
    int err = vfs_stat(none, p, false, st);
    if(err != -ENOENT){
        panic("test_fs: %s %s ar fi trebuit sa lipseasca (stat = %d)", fs.name, rel, err);
    }
}

// ---- fisierele noi (specificatia e oglindita in tests/check_fs.sh) ----
// seed = baza + 100 * fs.idx

struct NewFile {
    const char* rel;
    uint32_t seed;
    uint64_t size;
};

const NewFile NEWS[] = {
    {"new/kernel.txt", 11, 30},
    {"new/blob.bin", 12, 700001},
    {"new/sub/exact4k.bin", 13, 4096},
    {"new/Kernel Written Long Name.txt", 14, 1},
    {"new/renamed.bin", 15, 70000},
};

void join_fs(char* out, const Fs& fs, const char* rel){
    path_join(out, 128, fs.root, rel);
}

void phase1_write(const Fs& fs){
    char p[128], q[128];
    join_fs(p, fs, "new");
    EQ(vfs_mkdir(none, p, 0755), 0);
    join_fs(p, fs, "new/sub");
    EQ(vfs_mkdir(none, p, 0755), 0);
    static const uint32_t chunks[] = {30, 4093, 4096, 1, 70000};
    // renamed.bin se creeaza sub alt nume si se redenumeste; gone.bin se creeaza si se sterge
    for(uint32_t i = 0; i < sizeof(NEWS) / sizeof(NEWS[0]); i++){
        const char* rel = NEWS[i].rel;
        if(i == 4){
            rel = "new/tmp-name.bin";
        }
        join_fs(p, fs, rel);
        write_gen(p, NEWS[i].seed + 100 * fs.idx, NEWS[i].size, chunks[i]);
    }
    join_fs(p, fs, "new/gone.bin");
    write_gen(p, 99, 9000, 1000);
    join_fs(p, fs, "new/tmp-name.bin");
    join_fs(q, fs, "new/renamed.bin");
    EQ(vfs_rename(none, p, q), 0);
    join_fs(p, fs, "new/gone.bin");
    EQ(vfs_unlink(none, p), 0);
    // O_EXCL pe un fisier existent
    File* f;
    join_fs(p, fs, "new/kernel.txt");
    EQ(vfs_open(none, p, O_CREAT | O_EXCL | O_WRONLY, 0644, f), -EEXIST);
}

// Scrie NEW.SHA din fisierele de pe disc (citite inapoi, deci sha256-ul e al celor scrise efectiv)
void write_new_manifest(const Fs& fs){
    static char out[2048];
    uint64_t n = 0;
    for(uint32_t i = 0; i < sizeof(NEWS) / sizeof(NEWS[0]); i++){
        char p[128], hex[65];
        uint64_t size;
        join_fs(p, fs, NEWS[i].rel);
        file_sha(p, size, hex);
        EQ(size, NEWS[i].size);
        // verificare locala: acelasi sha256 cu cel al generatorului
        Sha256 s;
        sha_init(s);
        for(uint64_t o = 0; o < size; o += sizeof(buf)){
            uint64_t k = size - o < sizeof(buf) ? size - o : sizeof(buf);
            for(uint64_t j = 0; j < k; j++){
                buf[j] = gen_byte(NEWS[i].seed + 100 * fs.idx, o + j);
            }
            sha_update(s, buf, k);
        }
        char want[65];
        sha_final(s, want);
        CHECK(memcmp(hex, want, 65) == 0);
        uint64_t l = slen(NEWS[i].rel);
        CHECK(n + 64 + 24 + l + 2 < sizeof(out));
        memcpy(out + n, hex, 64);
        n += 64;
        out[n++] = ' ';
        char num[24];
        int d = 0;
        uint64_t v = size;
        do {
            num[d++] = (char)('0' + v % 10);
            v /= 10;
        } while(v);
        while(d){
            out[n++] = num[--d];
        }
        out[n++] = ' ';
        memcpy(out + n, NEWS[i].rel, l);
        n += l;
        out[n++] = '\n';
    }
    char p[128];
    join_fs(p, fs, "NEW.SHA");
    File* f = open_ok(p, O_CREAT | O_EXCL | O_WRONLY);
    wr(f, (const uint8_t*)out, n);
    file_put(f);
}

void phase2_check_new(const Fs& fs){
    CHECK(man_load(fs, "NEW.SHA"));
    CHECK(man_verify(fs) == sizeof(NEWS) / sizeof(NEWS[0]));
    must_missing(fs, "new/gone.bin");
    must_missing(fs, "new/tmp-name.bin");
}

void fs_task(void* arg){
    Job* j = (Job*)arg;
    sha_selftest();
    kprintf("[OK] test_fs SHA-256 (vectori cunoscuti)\n");

    // sda1 a fost montat pe /media/disk la boot
    Stat st;
    EQ(vfs_stat(none, "/media/disk", false, st), 0);
    bool phase2 = man_load(FS[0], "NEW.SHA");
    kprintf("test_fs: fase %d\n", phase2 ? 2 : 1);

    int err = vfs_mount(none, "sda2", "/mnt", "ext4", 0, phase2 ? "ro" : "rw");
    if(err){
        panic("test_fs: montarea ext4 sda2 = %d", err);
    }
    kprintf("[OK] test_fs montat: FAT32 pe /media/disk (sda1), ext4 pe /mnt (sda2, %s)\n", phase2 ? "ro" : "rw");

    for(const Fs& fs : FS){
        CHECK(man_load(fs, "MANIFEST.SHA"));
        CHECK(man_verify(fs) >= 10);
        kprintf("[OK] test_fs %s manifest initial verificat\n", fs.name);
    }
    if(!phase2){
        for(const Fs& fs : FS){
            must_missing(fs, "NEW.SHA");
            phase1_write(fs);
            write_new_manifest(fs);
            kprintf("[OK] test_fs %s fisiere noi scrise\n", fs.name);
        }
        // Citite inapoi din nou, dupa toate scrierile (ext4 inca in tranzactia curenta)
        for(const Fs& fs : FS){
            phase2_check_new(fs);
        }
        kprintf("[OK] test_fs fase 1 (scriere) terminata\n");
    } else {
        for(const Fs& fs : FS){
            phase2_check_new(fs);
            kprintf("[OK] test_fs %s fisiere noi regasite\n", fs.name);
        }
        kprintf("[OK] test_fs fase 2 (regasire) terminata\n");
    }
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(vfs_umount(none, "/media/disk"), 0);
    kprintf("[OK] test_fs demontat curat\n");
    j->done = true;
}

}

void test_fs_run(){
    Job job = {false};
    Task* t = task_create_kernel("t-fs", fs_task, &job);
    CHECK(t);
    sched_add(t);
    // La boot intreruperile sunt oprite, iar hlt-ul din sleep_ms le cere pornite
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 18000 && !job.done; i++){
        sleep_ms(50);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    kprintf("[OK] test_fs (FAT32 + ext4 pe acelasi disc GPT: SHA-256, scriere, regasire)\n");
}
