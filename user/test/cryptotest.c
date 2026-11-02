/* /bin/cryptotest: vectorii de test RFC 6234 (SHA-256), RFC 4231 (HMAC-SHA256), RFC 7914 sectiunea 11 (PBKDF2-HMAC-SHA256)
 * si formatul de hash $pbkdf2-sha256$ (lib/crypto, legat din libc.a). Tipareste "[OK] cryptotest", iese cu 0. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <crypto/pwhash.h>

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
