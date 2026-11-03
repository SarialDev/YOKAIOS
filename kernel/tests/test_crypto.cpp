#include "tests/test_crypto.h"
#include "crypto/pwhash.h"
#include "crypto/sha512.h"
#include "crypto/ed25519.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

#define CHECK(cond) do { if(!(cond)){ panic("test_crypto:%d: %s", __LINE__, #cond); } } while(0)

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

bool eq(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

void to_hex(const uint8_t* b, size_t n, char* out){
    static const char d[] = "0123456789abcdef";
    for(size_t i = 0; i < n; i++){
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

static void from_hex(const char *s, uint8_t *out, size_t n){
    for(size_t i = 0; i < n; i++){
        unsigned v = 0;
        for(int j = 0; j < 2; j++){
            char c = s[2 * i + j];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
}

static size_t strlen_(const char *s){
    size_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

// Vectorii principali, calculati de codul din kernel (acelasi lib/crypto ca in userland)
void kernel_side(){
    uint8_t d[32];
    char h[65];
    sha256("abc", 3, d);
    to_hex(d, 32, h);
    CHECK(eq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    to_hex(d, 32, h);
    CHECK(eq(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    uint8_t o[64];
    char hh[129];
    CHECK(pbkdf2_hmac_sha256("passwd", 6, "salt", 4, 1, o, 64) == 0);
    to_hex(o, 64, hh);
    CHECK(eq(hh, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));

    // hash cunoscut din /etc/shadow-ul de test ("hunter2") + gresit + stricat
    static const char known[] = "$pbkdf2-sha256$100000$000102030405060708090a0b0c0d0e0f$844d6e092def956a3d2773e41d721bf2d95a1d8f524d970ca76cd20528a03679";
    CHECK(pwhash_verify("hunter2", 7, known) == 1);
    CHECK(pwhash_verify("hunter3", 7, known) == 0);
    CHECK(pwhash_verify("hunter2", 7, "!") == -1);
    CHECK(crypto_ct_equal("abc", "abc", 3) == 1 && crypto_ct_equal("abc", "abd", 3) == 0);
    /* SHA-512 (FIPS 180-4) */
    {
        uint8_t d[64];
        char h[129];
        sha512("abc", 3, d);
        to_hex(d, 64, h);
        CHECK(eq(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
        sha512("", 0, d);
        to_hex(d, 64, h);
        CHECK(eq(h, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    }
    /* Ed25519, RFC 8032 sectiunea 7.1 (TEST 1..3): cheie publica, semnatura deterministica, verificare, respingere */
    {
        static const struct { const char *seed, *pub, *msg, *sig; } v[] = {
            {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
             "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
            {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
             "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
            {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
             "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
        };
        for(unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++){
            uint8_t seed[32], pub[32], pub2[32], sig[64], sig2[64], msg[2];
            from_hex(v[i].seed, seed, 32);
            from_hex(v[i].pub, pub, 32);
            from_hex(v[i].sig, sig, 64);
            size_t mn = strlen_(v[i].msg) / 2;
            from_hex(v[i].msg, msg, mn);
            ed25519_public_key(pub2, seed);
            CHECK(crypto_ct_equal(pub, pub2, 32) == 1);
            ed25519_sign(sig2, seed, "", 0, msg, mn);
            CHECK(crypto_ct_equal(sig, sig2, 64) == 1);
            CHECK(ed25519_verify(sig, pub, "", 0, msg, mn) == 0);
            /* contextul de domeniu e parte din mesaj: ctx="a" + msg == mesajul "a"||msg */
            ed25519_sign(sig2, seed, "a", 1, msg, mn);
            CHECK(ed25519_verify(sig2, pub, "a", 1, msg, mn) == 0 && ed25519_verify(sig2, pub, "", 0, msg, mn) == -1);
            sig[i] ^= 1;                                 /* R */
            CHECK(ed25519_verify(sig, pub, "", 0, msg, mn) == -1);
            sig[i] ^= 1;
            sig[40] ^= 1;                                /* S */
            CHECK(ed25519_verify(sig, pub, "", 0, msg, mn) == -1);
            sig[40] ^= 1;
            msg[0] ^= 1;
            if(mn){
                CHECK(ed25519_verify(sig, pub, "", 0, msg, mn) == -1);
            }
            pub[5] ^= 1;
            CHECK(ed25519_verify(sig, pub, "", 0, msg, mn) == -1);
        }
        /* S >= L (malleabilitate) se respinge: S' = S + L */
        uint8_t seed[32], pub[32], sig[64];
        from_hex(v[0].seed, seed, 32);
        from_hex(v[0].pub, pub, 32);
        from_hex(v[0].sig, sig, 64);
        static const uint8_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
        unsigned carry = 0;
        for(int i = 0; i < 32; i++){
            unsigned t = sig[32 + i] + L[i] + carry;
            sig[32 + i] = (uint8_t)t;
            carry = t >> 8;
        }
        CHECK(ed25519_verify(sig, pub, "", 0, "", 0) == -1);
    }
}

}

void test_crypto_run(){
    __asm__ volatile("sti");
    kernel_side();
    kprintf("[OK] test_crypto kernel\n");

    static const char* const argv[] = {"cryptotest", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn("/bin/cryptotest", argv, envp);
    if(!pid){
        panic("test_crypto: /bin/cryptotest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_crypto: cryptotest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_crypto: cryptotest a iesit cu %d (vezi liniile \"cryptotest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_crypto: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_crypto: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_crypto (SHA-256, HMAC, PBKDF2, pwhash: kernel + userland)\n");
}
