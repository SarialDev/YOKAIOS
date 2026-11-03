#include "tests/test_crypto.h"
#include "crypto/pwhash.h"
#include "crypto/sha512.h"
#include "crypto/ed25519.h"
#include "crypto/xts.h"
#include "crypto/aes.h"
#include "crypto/hmac.h"
#include "crypto/pbkdf2.h"
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

// Vectori oficiali: FIPS 180-4 / NIST CAVP (SHA-256), RFC 4231 (HMAC-SHA256), PBKDF2-HMAC-SHA256 (RFC 7914 + lista RFC 6070 refacuta
// cu SHA-256), RFC 8032 7.1 (Ed25519, inclusiv "SHA(abc)"), IEEE 1619-2007 anexa B (AES-XTS)
bool hex_is(const uint8_t* b, size_t n, const char* want){
    static char h[257];
    if(n > 128){
        return false;
    }
    to_hex(b, n, h);
    return eq(h, want);
}

void official_sha256(){
    uint8_t d[32];
    sha256("", 0, d);
    CHECK(hex_is(d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const char* m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m448, strlen_(m448), d);
    CHECK(hex_is(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char* m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha256(m896, strlen_(m896), d);
    CHECK(hex_is(d, 32, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    // un milion de 'a', in bucati de 1000 si de 7 octeti (cealalta limita de bloc)
    static char a[1000];
    for(unsigned i = 0; i < sizeof(a); i++){
        a[i] = 'a';
    }
    sha256_ctx c;
    sha256_init(&c);
    for(int i = 0; i < 1000; i++){
        sha256_update(&c, a, 1000);
    }
    sha256_final(&c, d);
    CHECK(hex_is(d, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    sha256_init(&c);
    for(int left = 1000000; left > 0; ){
        int n = left < 7 ? left : 7;
        sha256_update(&c, a, (size_t)n);
        left -= n;
    }
    sha256_final(&c, d);
    CHECK(hex_is(d, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

void official_hmac(){
    uint8_t k[131], m[150], d[32];
    // RFC 4231 caz 1
    for(int i = 0; i < 20; i++){ k[i] = 0x0b; }
    hmac_sha256(k, 20, "Hi There", 8, d);
    CHECK(hex_is(d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    // caz 3
    for(int i = 0; i < 20; i++){ k[i] = 0xaa; }
    for(int i = 0; i < 50; i++){ m[i] = 0xdd; }
    hmac_sha256(k, 20, m, 50, d);
    CHECK(hex_is(d, 32, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"));
    // caz 4
    for(int i = 0; i < 25; i++){ k[i] = (uint8_t)(i + 1); }
    for(int i = 0; i < 50; i++){ m[i] = 0xcd; }
    hmac_sha256(k, 25, m, 50, d);
    CHECK(hex_is(d, 32, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"));
    // cazurile 6 si 7: cheie de 131 de octeti (mai lunga decat blocul: se hash-uieste intai)
    for(int i = 0; i < 131; i++){ k[i] = 0xaa; }
    const char* m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(k, 131, m6, strlen_(m6), d);
    CHECK(hex_is(d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
    const char* m7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    hmac_sha256(k, 131, m7, strlen_(m7), d);
    CHECK(hex_is(d, 32, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"));
    // varianta incrementala = cea dintr-o bucata
    hmac_sha256_ctx h;
    hmac_sha256_init(&h, k, 131);
    hmac_sha256_update(&h, m7, 10);
    hmac_sha256_update(&h, m7 + 10, strlen_(m7) - 10);
    hmac_sha256_final(&h, d);
    CHECK(hex_is(d, 32, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"));
}

void official_pbkdf2(){
    uint8_t o[40];
    CHECK(pbkdf2_hmac_sha256("password", 8, "salt", 4, 1, o, 32) == 0);
    CHECK(hex_is(o, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    CHECK(pbkdf2_hmac_sha256("password", 8, "salt", 4, 2, o, 32) == 0);
    CHECK(hex_is(o, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    CHECK(pbkdf2_hmac_sha256("password", 8, "salt", 4, 4096, o, 32) == 0);
    CHECK(hex_is(o, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
    // parola si salt-ul mai lungi decat blocul de iesire, dkLen = 40 (doua blocuri)
    CHECK(pbkdf2_hmac_sha256("passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, o, 40) == 0);
    CHECK(hex_is(o, 40, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"));
    // argumente invalide
    CHECK(pbkdf2_hmac_sha256("p", 1, "s", 1, 0, o, 32) == -1);
}

void official_ed25519(){
    // RFC 8032 7.1 "TEST SHA(abc)": mesajul = SHA-512("abc") (64 de octeti)
    uint8_t seed[32], pub[32], pub2[32], sig[64], sig2[64], msg[64];
    from_hex("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42", seed, 32);
    from_hex("ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf", pub, 32);
    from_hex("dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704", sig, 64);
    sha512("abc", 3, msg);
    ed25519_public_key(pub2, seed);
    CHECK(crypto_ct_equal(pub, pub2, 32) == 1);
    ed25519_sign(sig2, seed, "", 0, msg, 64);
    CHECK(crypto_ct_equal(sig, sig2, 64) == 1);
    CHECK(ed25519_verify(sig, pub, "", 0, msg, 64) == 0);
    for(int bit = 0; bit < 512; bit += 37){          // orice bit schimbat in semnatura => respinsa
        sig[bit >> 3] ^= (uint8_t)(1 << (bit & 7));
        CHECK(ed25519_verify(sig, pub, "", 0, msg, 64) == -1);
        sig[bit >> 3] ^= (uint8_t)(1 << (bit & 7));
    }
    CHECK(ed25519_verify(sig, pub, "", 0, msg, 64) == 0);
}

void xts_case(const char* k1, const char* k2, size_t klen, uint64_t sector, const uint8_t* pt, size_t len, const char* want_head, size_t head){
    static uint8_t key[64], ct[512], back[512];
    from_hex(k1, key, klen);
    from_hex(k2, key + klen, klen);
    xts_ctx x;
    CHECK(xts_set_key(&x, key, 2 * klen) == 0);
    CHECK(xts_encrypt(&x, sector, pt, ct, len) == 0);
    CHECK(hex_is(ct, head, want_head));
    CHECK(xts_decrypt(&x, sector, ct, back, len) == 0);
    CHECK(memcmp(back, pt, len) == 0);
    CHECK(xts_encrypt(&x, sector + 1, pt, back, len) == 0 && memcmp(back, ct, len) != 0);   // alt sector, alt ciphertext
    xts_wipe(&x);
}

void official_xts(){
    static uint8_t p44[32], p512[512];
    for(int i = 0; i < 32; i++){ p44[i] = 0x44; }
    for(int i = 0; i < 512; i++){ p512[i] = (uint8_t)i; }
    // cheile egale (vectorul 1 din IEEE 1619) sunt refuzate de standard
    {
        uint8_t z[32] = {0};
        xts_ctx x;
        CHECK(xts_set_key(&x, z, 32) == -1);
    }
    // de doua ori: cu AES-NI daca exista, apoi fortat software (acelasi rezultat)
    for(int pass = 0; pass < 2; pass++){
        aes_force_software(pass);
        // IEEE 1619 vectorul 2
        xts_case("11111111111111111111111111111111", "22222222222222222222222222222222", 16, 0x3333333333ULL, p44, 32,
                 "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0", 32);
        // vectorul 3
        xts_case("fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0", "22222222222222222222222222222222", 16, 0x3333333333ULL, p44, 32,
                 "af85336b597afc1a900b2eb21ec949d292df4c047e0b21532186a5971a227a89", 32);
        // vectorul 4: XTS-AES-128, 512 de octeti 
        xts_case("27182818284590452353602874713526", "31415926535897932384626433832795", 16, 0, p512, 512,
                 "27a7479befa1d476489f308cd4cfa6e2a96e4bbe3208ff25287dd3819616e89c", 32);
        // vectorul 10: XTS-AES-256, tweak 0xff
        xts_case("2718281828459045235360287471352662497757247093699959574966967627",
                 "3141592653589793238462643383279502884197169399375105820974944592", 32, 0xff, p512, 512,
                 "1c3b3a102f770386e4836c99e370cf9bea00803f5e482357a4ae12d414a3e63b", 32);
    }
    aes_force_software(0);
    // FIPS 197 C.1 (AES-128) si C.3 (AES-256)
    uint8_t key[32], pt[16], ct[16];
    from_hex("000102030405060708090a0b0c0d0e0f", key, 16);
    from_hex("00112233445566778899aabbccddeeff", pt, 16);
    aes_ctx a;
    CHECK(aes_set_key(&a, key, 16) == 0);
    aes_encrypt_blocks(&a, pt, ct, 1);
    CHECK(hex_is(ct, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    CHECK(aes_set_key(&a, key, 32) == 0);
    aes_encrypt_blocks(&a, pt, ct, 1);
    CHECK(hex_is(ct, 16, "8ea2b7ca516745bfeafc49904b496089"));
}

void official_vectors(){
    official_sha256();
    official_hmac();
    official_pbkdf2();
    official_ed25519();
    official_xts();
}

// /bin/sectest: /etc/shadow ilizibil pentru 1000, pachet cu semnatura gresita respins, fara CAP_RT refuz, ASLR
int run_user(const char* path, const char* const* argv){
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn(path, argv, envp);
    if(!pid){
        panic("test_crypto: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_crypto: %s (pid %lu) a disparut fara exit", path, pid);
    }
    if(process_count() != procs_before){
        panic("test_crypto: %ld procese ramase dupa %s", (int64_t)(process_count() - procs_before), path);
    }
    if(pmm_free_count() + 24 < free_before){
        panic("test_crypto: %ld pagini pierdute dupa %s", (int64_t)(free_before - pmm_free_count()), path);
    }
    return code;
}

}

void test_crypto_run(){
    __asm__ volatile("sti");
    kernel_side();
    kprintf("[OK] test_crypto kernel\n");
    official_vectors();
    kprintf("[OK] test_crypto official vectors (SHA-256, HMAC, PBKDF2, Ed25519, AES-XTS)\n");

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

    static const char* const sargv[] = {"sectest", nullptr};
    int scode = run_user("/bin/sectest", sargv);
    if(scode != 0){
        panic("test_crypto: sectest a iesit cu %d (vezi liniile \"sectest: FAIL\")", scode);
    }
    kprintf("[OK] test_crypto security (shadow, semnatura, CAP_RT, ASLR)\n");
}
