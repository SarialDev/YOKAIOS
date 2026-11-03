#include "tests/test_crypt.h"
#include "block/crypt.h"
#include "fs/vfs/vfs.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "time/clock.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "hal/block.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"
#include "crypto/aes.h"
#include "crypto/xts.h"
#include "crypto/chacha20.h"

namespace {

#define CHECK(cond) do { if(!(cond)){ panic("test_crypt:%d: %s", __LINE__, #cond); } } while(0)
#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_crypt:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)

const Path none = {};

uint64_t slen(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

int scmp(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}
const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

struct Job {
    volatile bool done;
};

// ---- vectori ----

void from_hex(const char* s, uint8_t* out, size_t n){
    for(size_t i = 0; i < n; i++){
        unsigned v = 0;
        for(int j = 0; j < 2; j++){
            char c = s[2 * i + j];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
}

bool hex_eq(const uint8_t* got, const char* hex, size_t n){
    uint8_t want[128];
    CHECK(n <= sizeof(want));
    from_hex(hex, want, n);
    return memcmp(got, want, n) == 0;
}

void vectors(){
    uint8_t key[32], pt[16], ct[16], back[16];
    for(int i = 0; i < 32; i++){
        key[i] = (uint8_t)i;
    }
    for(int i = 0; i < 16; i++){
        pt[i] = (uint8_t)(i * 0x11);
    }
    // FIPS 197 C.1 (AES-128) si C.3 (AES-256), de doua ori: cu AES-NI daca exista si software fortat
    for(int pass = 0; pass < 2; pass++){
        aes_force_software(pass);
        aes_ctx c;
        CHECK(aes_set_key(&c, key, 16) == 0);
        aes_encrypt_blocks(&c, pt, ct, 1);
        CHECK(hex_eq(ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16));
        aes_decrypt_blocks(&c, ct, back, 1);
        CHECK(memcmp(back, pt, 16) == 0);
        CHECK(aes_set_key(&c, key, 32) == 0);
        aes_encrypt_blocks(&c, pt, ct, 1);
        CHECK(hex_eq(ct, "8ea2b7ca516745bfeafc49904b496089", 16));
        aes_decrypt_blocks(&c, ct, back, 1);
        CHECK(memcmp(back, pt, 16) == 0);
        CHECK(aes_set_key(&c, key, 24) == 0);
        aes_encrypt_blocks(&c, pt, ct, 1);
        CHECK(hex_eq(ct, "dda97ca4864cdfe06eaf70a0ec0d7191", 16));
        CHECK(aes_set_key(&c, key, 20) == -1);
    }
    aes_force_software(0);
    // S-box in timp constant == tabela standard (cateva puncte cunoscute: 00 -> 63, 01 -> 7c, 53 -> ed, ff -> 16)
    CHECK(aes_sbox_ct(0x00) == 0x63 && aes_sbox_ct(0x01) == 0x7c && aes_sbox_ct(0x53) == 0xed && aes_sbox_ct(0xff) == 0x16);

    // XTS-AES-256, sector 12345, 512 de octeti: vector calculat cu o implementare independenta (OpenSSL/cryptography)
    uint8_t k64[64], buf[512], out[512];
    for(int i = 0; i < 64; i++){
        k64[i] = (uint8_t)(i * 3 + 1);
    }
    for(int i = 0; i < 512; i++){
        buf[i] = (uint8_t)(i * 7);
    }
    for(int pass = 0; pass < 2; pass++){
        aes_force_software(pass);
        xts_ctx x;
        CHECK(xts_set_key(&x, k64, 64) == 0);
        CHECK(xts_encrypt(&x, 12345, buf, out, 512) == 0);
        CHECK(hex_eq(out, "81fa532f792428539fe74fc13376e49789a9c99d174e1335507e84991349a0f2", 32));
        CHECK(hex_eq(out + 480, "1eff625053af05d3185fe8ab82b0e9f894e7dd45563c3fc5a9c71dc5c033b6b7", 32));
        CHECK(xts_decrypt(&x, 12345, out, out, 512) == 0);
        CHECK(memcmp(out, buf, 512) == 0);
        CHECK(xts_encrypt(&x, 12345, buf, out, 100) == -1);         // nu e multiplu de 16
        uint8_t same[64];
        memcpy(same, k64, 32);
        memcpy(same + 32, k64, 32);
        CHECK(xts_set_key(&x, same, 64) == -1);                     // K1 == K2 se refuza
    }
    aes_force_software(0);
    kprintf("[OK] test_crypt AES-128/192/256 (FIPS 197) si AES-XTS (vector independent); AES-NI %s\n",
            aes_hw_available() ? "prezent: hardware == software" : "absent: doar software constant-time");

    // ChaCha20, RFC 8439 2.3.2: bloc cu contor 1
    uint8_t nonce[12] = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0}, blk[64];
    chacha20_block(key, 1, nonce, blk);
    CHECK(hex_eq(blk, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64));
    // XOR de doua ori = identitate, pe lungime care nu e multiplu de 64
    uint8_t a[150], b[150];
    for(int i = 0; i < 150; i++){
        a[i] = (uint8_t)i;
    }
    chacha20_xor(key, 7, nonce, a, b, 150);
    CHECK(memcmp(a, b, 150) != 0);
    chacha20_xor(key, 7, nonce, b, b, 150);
    CHECK(memcmp(a, b, 150) == 0);
    kprintf("[OK] test_crypt ChaCha20 (RFC 8439)\n");
}

// ---- proces /bin/cryptsetup ----

int run(const char* a1, const char* a2 = nullptr, const char* a3 = nullptr, const char* a4 = nullptr,
        const char* a5 = nullptr, const char* a6 = nullptr, const char* a7 = nullptr, const char* a8 = nullptr,
        const char* a9 = nullptr, const char* a10 = nullptr, const char* a11 = nullptr){
    const char* argv[16] = {"cryptsetup", a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, nullptr};
    uint64_t pid = exec_spawn("/bin/cryptsetup", argv, envp);
    if(!pid){
        panic("test_crypt: /bin/cryptsetup nu a pornit");
    }
    int code = -1;
    for(int i = 0; i < 6000 && !process_exit_status(pid, code); i++){
        sleep_ms(10);
    }
    CHECK(code != -1);
    return code;
}

BlockDevice* find_dev(const char* name){
    for(int k = 0; k < hal_count(HalKind::Block); k++){
        const char* n = hal_name(HalKind::Block, k);
        if(!scmp(n, name)){
            return hal_block(k);
        }
    }
    return nullptr;
}

// ---- disc in RAM ----

const uint32_t RAM_SECTORS = 2048;      // 1 MiB: antet 4 KiB + 2040 de sectoare
uint8_t ram_data[RAM_SECTORS * 512];

class RamDisk final : public BlockDevice {
public:
    uint32_t block_size() const override { return 512; }
    uint64_t block_count() const override { return RAM_SECTORS; }
    int read(uint64_t lba, uint32_t n, void* buf) override {
        if(lba + n > RAM_SECTORS){
            return -EINVAL;
        }
        memcpy(buf, ram_data + lba * 512, (uint64_t)n * 512);
        return 0;
    }
    int write(uint64_t lba, uint32_t n, const void* buf) override {
        if(lba + n > RAM_SECTORS){
            return -EINVAL;
        }
        memcpy(ram_data + lba * 512, buf, (uint64_t)n * 512);
        return 0;
    }
};

RamDisk ramdisk;

const char MARKER[] = "RAMSECRET-PLAINTEXT-MARKER: the quick brown fox jumps over the lazy dog 0123456789. ";

bool ram_contains(const char* needle, uint64_t len){
    for(uint64_t i = 0; i + len <= sizeof(ram_data); i++){
        if(ram_data[i] == (uint8_t)needle[0] && memcmp(ram_data + i, needle, len) == 0){
            return true;
        }
    }
    return false;
}

uint8_t sbuf[8 * 512], rbuf[8 * 512];

void fill_marker(uint8_t* p, uint64_t n){
    uint64_t l = sizeof(MARKER) - 1;
    for(uint64_t i = 0; i < n; i++){
        p[i] = (uint8_t)MARKER[i % l];
    }
}

void ram_cipher_test(const char* cipher, const char* pw, const char* pw2){
    memset(ram_data, 0, sizeof(ram_data));
    EQ(run("format", "-y", "--insecure-rng", "-i", "1000", "-c", cipher, "-p", pw, "/dev/ram0"), 0);
    EQ(run("dump", "/dev/ram0"), 0);
    CHECK(memcmp(ram_data, "YKCRYPT1", 8) == 0);
    // parola gresita: refuzata, nimic nu devine activ
    CHECK(run("open", "-p", "gresita", "/dev/ram0", "crypt0") != 0);
    CHECK(run("status", "crypt0") != 0);
    // parola corecta
    EQ(run("open", "-p", pw, "/dev/ram0", "crypt0"), 0);
    EQ(run("status", "crypt0"), 0);
    BlockDevice* cd = find_dev("crypt0");
    CHECK(cd);
    EQ(cd->block_count(), 2040);
    CHECK(run("open", "-p", pw, "/dev/ram0", "crypt1") != 0);          // acelasi parinte: ocupat
    CHECK(run("open", "-p", pw, "/dev/crypt0", "crypt1") != 0);        // nu se stivuieste
    // scrie acelasi text in 8 sectoare (lba 5..12), citeste-l inapoi
    fill_marker(sbuf, sizeof(sbuf));
    EQ(cd->write(5, 8, sbuf), 0);
    EQ(cd->read(5, 8, rbuf), 0);
    CHECK(memcmp(sbuf, rbuf, sizeof(sbuf)) == 0);
    EQ(cd->read(2040, 1, rbuf), -EINVAL);                               // dincolo de capat
    // pe discul brut textul nu apare, sectoarele identice au criptotexte diferite, antetul nu contine parola
    CHECK(!ram_contains(MARKER, 24));
    CHECK(!ram_contains(pw, slen(pw)));
    uint64_t base = 8 + 5;                                              // sector pe ram0 = 8 (antet) + lba
    CHECK(memcmp(ram_data + base * 512, ram_data + (base + 1) * 512, 512) != 0);
    CHECK(memcmp(ram_data + base * 512, sbuf, 512) != 0);
    // inchis: dispozitivul ramane in HAL dar raspunde -ENODEV; cheia nu mai e in kernel
    EQ(run("close", "crypt0"), 0);
    EQ(cd->block_count(), 0);
    EQ(cd->read(5, 1, rbuf), -ENODEV);
    CHECK(run("close", "crypt0") != 0);
    // redeschis: datele raman; --soft (fara AES-NI) citeste la fel ce a scris varianta hardware
    EQ(run("open", "-p", pw, "/dev/ram0", "crypt0"), 0);
    memset(rbuf, 0, sizeof(rbuf));
    EQ(cd->read(5, 8, rbuf), 0);
    CHECK(memcmp(sbuf, rbuf, sizeof(sbuf)) == 0);
    EQ(run("close", "crypt0"), 0);
    EQ(run("open", "-p", pw, "--soft", "/dev/ram0", "crypt0"), 0);
    memset(rbuf, 0, sizeof(rbuf));
    EQ(cd->read(5, 8, rbuf), 0);
    CHECK(memcmp(sbuf, rbuf, sizeof(sbuf)) == 0);
    EQ(run("close", "crypt0"), 0);
    // schimbarea parolei: aceeasi cheie master, datele raman
    CHECK(run("passwd", "-p", "gresita", "-n", pw2, "-i", "1000", "/dev/ram0") != 0);
    EQ(run("passwd", "-p", pw, "-n", pw2, "-i", "1500", "/dev/ram0"), 0);
    CHECK(run("open", "-p", pw, "/dev/ram0", "crypt0") != 0);
    EQ(run("open", "-p", pw2, "/dev/ram0", "crypt0"), 0);
    memset(rbuf, 0, sizeof(rbuf));
    EQ(cd->read(5, 8, rbuf), 0);
    CHECK(memcmp(sbuf, rbuf, sizeof(sbuf)) == 0);
    EQ(run("close", "crypt0"), 0);
    // un antet modificat (un bit in salt) nu se mai deschide
    ram_data[60] ^= 1;
    CHECK(run("open", "-p", pw2, "/dev/ram0", "crypt0") != 0);
    ram_data[60] ^= 1;
    EQ(run("open", "-p", pw2, "/dev/ram0", "crypt0"), 0);
    EQ(run("close", "crypt0"), 0);
    kprintf("[OK] test_crypt ram0 %s: format, open, parola gresita, date brute fara text, close, --soft, passwd, antet modificat\n", cipher);
}

// ---- ext4 pe sda criptat de gazda ----

const char ORIG_PATH[] = "/mnt/secrets/plan.txt";
const char ORIG_TEXT[] = "PLAINTEXT-ORIGINAL-SECRET: the quick brown fox 0123456789\n";
const char NEW_PATH[] = "/mnt/runtime.txt";
const char NEW_TEXT[] = "TOPSECRET-WRITTEN-AT-RUNTIME: pack my box with five dozen liquor jugs\n";
const char BULK_PATH[] = "/mnt/bulk.txt";
const uint32_t BULK_LINES = 8192;       // 64 de octeti pe linie = 512 KiB

uint8_t big[65536];

void bulk_line(uint32_t i, char* out){
    memcpy(out, "TOPSECRET-BULK-", 15);
    for(int d = 6; d >= 0; d--){
        out[15 + d] = (char)('0' + i % 10);
        i /= 10;
    }
    out[22] = ' ';
    memset(out + 23, 'x', 40);
    out[63] = '\n';
}

File* open_ok(const char* p, int flags){
    File* f;
    int err = vfs_open(none, p, flags, 0644, f);
    if(err){
        panic("test_crypt: open %s = %d", p, err);
    }
    return f;
}

void read_expect(const char* p, const char* text){
    File* f = open_ok(p, O_RDONLY);
    uint64_t n = slen(text);
    EQ(f->ops->read(f, big, sizeof(big), false), (int64_t)n);
    CHECK(memcmp(big, text, n) == 0);
    EQ(f->ops->read(f, big, sizeof(big), false), 0);
    file_put(f);
}

void bulk_verify(){
    File* f = open_ok(BULK_PATH, O_RDONLY);
    uint32_t line = 0;
    uint64_t have = 0;
    for(;;){
        int64_t r = f->ops->read(f, big + have, sizeof(big) - have, false);
        CHECK(r >= 0);
        if(r == 0){
            break;
        }
        have += (uint64_t)r;
        uint64_t used = 0;
        while(have - used >= 64){
            char want[64];
            bulk_line(line++, want);
            CHECK(memcmp(big + used, want, 64) == 0);
            used += 64;
        }
        memmove(big, big + used, have - used);
        have -= used;
    }
    CHECK(have == 0);
    EQ(line, BULK_LINES);
    file_put(f);
}

void ext4_test(const char* pw){
    EQ(run("open", "-p", pw, "/dev/sda", "crypt2"), 0);
    EQ(run("status", "crypt2"), 0);
    EQ(vfs_mount(none, "crypt2", "/mnt", "ext4", 0, "rw"), 0);
    kprintf("[OK] test_crypt ext4 montat rw pe crypt2 (sda criptat)\n");
    read_expect(ORIG_PATH, ORIG_TEXT);
    // a doua deschidere / montare / parinte direct in timp ce volumul e in uz
    CHECK(run("close", "crypt2") != 0);
    CHECK(run("open", "-p", pw, "/dev/sda", "crypt3") != 0);
    // scrieri: un fisier mic si unul de 512 KiB (sute de sectoare criptate + jurnalul JBD2)
    File* f = open_ok(NEW_PATH, O_CREAT | O_EXCL | O_WRONLY);
    EQ(f->ops->write(f, NEW_TEXT, slen(NEW_TEXT), false), (int64_t)slen(NEW_TEXT));
    file_put(f);
    f = open_ok(BULK_PATH, O_CREAT | O_EXCL | O_WRONLY);
    for(uint32_t l = 0; l < BULK_LINES;){
        uint32_t n = 0;
        for(; n < sizeof(big) / 64 && l < BULK_LINES; n++, l++){
            bulk_line(l, (char*)big + 64 * n);
        }
        EQ(f->ops->write(f, big, 64 * n, false), 64 * n);
    }
    file_put(f);
    read_expect(NEW_PATH, NEW_TEXT);
    bulk_verify();
    kprintf("[OK] test_crypt ext4 pe volum criptat: citit fisier existent, scris runtime.txt + bulk.txt (512 KiB)\n");
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(run("close", "crypt2"), 0);
    // redeschis: totul regasit (ro)
    EQ(run("open", "-p", pw, "/dev/sda", "crypt2"), 0);
    EQ(vfs_mount(none, "crypt2", "/mnt", "ext4", 0, "ro"), 0);
    read_expect(ORIG_PATH, ORIG_TEXT);
    read_expect(NEW_PATH, NEW_TEXT);
    bulk_verify();
    EQ(vfs_umount(none, "/mnt"), 0);
    EQ(run("close", "crypt2"), 0);
    kprintf("[OK] test_crypt ext4 dupa inchidere si redeschidere: datele sunt acolo\n");
}

void crypt_task(void* arg){
    Job* j = (Job*)arg;
    CHECK(hal_register(HalKind::Block, &ramdisk, "ram0"));
    ram_cipher_test("xts", "correct horse battery staple", "Tr0ub4dor&3");
    ram_cipher_test("chacha20", "another passphrase", "yet another one");
    if(find_dev("sda")){
        ext4_test("sda-disk-passphrase");
    } else {
        kprintf("test_crypt: fara disc sda (DISK=...), sar peste testul ext4\n");
    }
    j->done = true;
}

}

void test_crypt_run(){
    vectors();
    Job job = {false};
    Task* t = task_create_kernel("t-crypt", crypt_task, &job);
    CHECK(t);
    sched_add(t);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 9000 && !job.done; i++){
        sleep_ms(50);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    kprintf("[OK] test_crypt (AES-XTS + ChaCha20: vectori, ram0, ext4 pe disc criptat)\n");
}
