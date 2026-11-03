/* /bin/cryptotest: vectorii de test RFC 6234 (SHA-256), RFC 4231 (HMAC-SHA256), RFC 7914 sectiunea 11 (PBKDF2-HMAC-SHA256)
 * si formatul de hash $pbkdf2-sha256$ (lib/crypto, legat din libc.a). Tipareste "[OK] cryptotest", iese cu 0. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <crypto/pwhash.h>
#include <crypto/sha512.h>
#include <crypto/ed25519.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "cryptotest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while(0)

static void hex(const uint8_t *b, size_t n, char *out){
    static const char d[] = "0123456789abcdef";
    for(size_t i = 0; i < n; i++){
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

static int sha_is(const void *m, size_t n, const char *want){
    uint8_t d[32];
    char h[65];
    sha256(m, n, d);
    hex(d, 32, h);
    return strcmp(h, want) == 0;
}

static int hmac_is(const void *k, size_t kn, const void *m, size_t mn, const char *want){
    uint8_t d[32];
    char h[65];
    hmac_sha256(k, kn, m, mn, d);
    hex(d, 32, h);
    return strcmp(h, want) == 0;
}

static int pbkdf2_is(const char *pw, const char *salt, uint32_t it, size_t dk, const char *want){
    uint8_t o[64];
    char h[129];
    if(dk > sizeof(o) || pbkdf2_hmac_sha256(pw, strlen(pw), salt, strlen(salt), it, o, dk) < 0){
        return 0;
    }
    hex(o, dk, h);
    return strcmp(h, want) == 0;
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

static int eq(const char *a, const char *b){
    return strcmp(a, b) == 0;
}

static void to_hex(const uint8_t *b, size_t n, char *out){
    hex(b, n, out);
}

static void test_ed25519(void){
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

static void test_sha256(void){
    CHECK(sha_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha_is(two, strlen(two), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char *four = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    CHECK(sha_is(four, strlen(four), "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    /* 1.000.000 x 'a', in felii de 1000 de octeti (test de update incremental) */
    char a[1000];
    memset(a, 'a', sizeof(a));
    sha256_ctx c;
    uint8_t d[32];
    char h[65];
    sha256_init(&c);
    for(int i = 0; i < 1000; i++){
        sha256_update(&c, a, sizeof(a));
    }
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    /* acelasi mesaj, taiat in bucati neregulate */
    sha256_init(&c);
    sha256_update(&c, two, 1);
    sha256_update(&c, two + 1, 54);
    sha256_update(&c, two + 55, 1);
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

static void test_hmac(void){
    uint8_t k[131];
    memset(k, 0x0b, 20);
    CHECK(hmac_is(k, 20, "Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK(hmac_is("Jefe", 4, "what do ya want for nothing?", 28, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    uint8_t data[50];
    memset(k, 0xaa, 20);
    memset(data, 0xdd, 50);
    CHECK(hmac_is(k, 20, data, 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"));
    memset(k, 0xaa, 131);       /* cheie mai lunga decat blocul */
    CHECK(hmac_is(k, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void test_pbkdf2(void){
    CHECK(pbkdf2_is("passwd", "salt", 1, 64,
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
    CHECK(pbkdf2_is("Password", "NaCl", 80000, 64,
          "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"));
    uint8_t o[8];
    CHECK(pbkdf2_hmac_sha256("a", 1, "s", 1, 0, o, sizeof(o)) == -1);
}

static void test_pwhash(void){
    uint8_t salt[PWHASH_SALT_LEN];
    char s[PWHASH_MAX], t[PWHASH_MAX];
    for(int i = 0; i < PWHASH_SALT_LEN; i++){
        salt[i] = (uint8_t)(i * 7 + 1);
    }
    CHECK(pwhash_make("parola-buna", 11, salt, PWHASH_ITER_MIN, s, sizeof(s)) == 0);
    CHECK(strncmp(s, "$pbkdf2-sha256$100000$", 22) == 0 && strlen(s) == 22 + 32 + 1 + 64);
    CHECK(pwhash_valid(s));
    CHECK(pwhash_verify("parola-buna", 11, s) == 1);
    CHECK(pwhash_verify("parola-rea", 10, s) == 0);
    CHECK(pwhash_verify("parola-bun", 10, s) == 0);
    CHECK(pwhash_verify("", 0, s) == 0);
    /* acelasi secret + aceeasi sare = acelasi sir; alta sare = alt sir */
    CHECK(pwhash_make("parola-buna", 11, salt, PWHASH_ITER_MIN, t, sizeof(t)) == 0 && strcmp(s, t) == 0);
    salt[0] ^= 1;
    CHECK(pwhash_make("parola-buna", 11, salt, PWHASH_ITER_MIN, t, sizeof(t)) == 0 && strcmp(s, t) != 0);
    /* respinse: prea putine iteratii, buffer mic */
    CHECK(pwhash_make("x", 1, salt, PWHASH_ITER_MIN - 1, t, sizeof(t)) == -1);
    CHECK(pwhash_make("x", 1, salt, PWHASH_ITER_MIN, t, PWHASH_MAX - 1) == -1);
    /* siruri care nu sunt hash-uri */
    CHECK(pwhash_verify("x", 1, "*") == -1 && pwhash_verify("x", 1, "!") == -1 && pwhash_verify("x", 1, "") == -1);
    CHECK(pwhash_verify("x", 1, "$pbkdf2-sha256$0$00000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000") == -1);
    CHECK(pwhash_verify("x", 1, "$pbkdf2-sha256$100000$zz0102030405060708090a0b0c0d0e0f$844d6e092def956a3d2773e41d721bf2d95a1d8f524d970ca76cd20528a03679") == -1);
    memcpy(t, s, sizeof(t));
    t[strlen(t) - 1] = 'g';
    CHECK(pwhash_verify("parola-buna", 11, t) == -1);
    /* hash cunoscut (fara iteratii multe), calculat in afara sistemului: "hunter2" */
    CHECK(pwhash_verify("hunter2", 7, "$pbkdf2-sha256$100000$000102030405060708090a0b0c0d0e0f$844d6e092def956a3d2773e41d721bf2d95a1d8f524d970ca76cd20528a03679") == 1);
    CHECK(crypto_ct_equal("abc", "abc", 3) == 1 && crypto_ct_equal("abc", "abd", 3) == 0 && crypto_ct_equal("", "", 0) == 1);
}

int main(void){
    test_sha256();
    test_ed25519();
    test_hmac();
    test_pbkdf2();
    test_pwhash();
    if(failed){
        printf("cryptotest: %d din %d verificari au esuat\n", failed, checks);
        return 1;
    }
    printf("[OK] cryptotest (%d verificari)\n", checks);
    return 0;
}
