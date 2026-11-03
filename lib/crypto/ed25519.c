/* Ed25519 standard (SHA-512) cu elementele de baza EdDSA din Monocypher: crypto_eddsa_reduce / mul_add / scalarbase /
 * check_equation. Monocypher insusi foloseste BLAKE2b, de aceea hash-ul se face aici. */
#include <crypto/ed25519.h>
#include <crypto/sha512.h>
#include "../../third_party/monocypher/monocypher.h"

/* h = SHA-512(a || b || ctx || m) mod L; mesajul semnat este ctx || m (ctx = prefix de separare a domeniilor) */
static void hash_reduce(uint8_t out[32], const void *a, size_t an, const void *b, size_t bn,
                        const void *x, size_t xn, const void *m, size_t mn){
    sha512_ctx c;
    uint8_t h[64];
    sha512_init(&c);
    sha512_update(&c, a, an);
    sha512_update(&c, b, bn);
    sha512_update(&c, x, xn);
    sha512_update(&c, m, mn);
    sha512_final(&c, h);
    crypto_eddsa_reduce(out, h);
}

int ed25519_verify(const uint8_t sig[64], const uint8_t pub[32], const void *ctx, size_t ctx_len,
                   const void *msg, size_t len){
    uint8_t h[32];
    hash_reduce(h, sig, 32, pub, 32, ctx, ctx_len, msg, len);
    return crypto_eddsa_check_equation(sig, pub, h) == 0 ? 0 : -1;
}

static void expand(uint8_t a[32], uint8_t prefix[32], const uint8_t seed[32]){
    uint8_t h[64];
    sha512(seed, 32, h);
    crypto_eddsa_trim_scalar(a, h);
    for(int i = 0; i < 32; i++){
        prefix[i] = h[32 + i];
    }
    crypto_wipe(h, sizeof(h));
}

void ed25519_public_key(uint8_t pub[32], const uint8_t seed[32]){
    uint8_t a[32], prefix[32];
    expand(a, prefix, seed);
    crypto_eddsa_scalarbase(pub, a);
    crypto_wipe(a, 32);
    crypto_wipe(prefix, 32);
}

void ed25519_sign(uint8_t sig[64], const uint8_t seed[32], const void *ctx, size_t ctx_len, const void *msg, size_t len){
    uint8_t a[32], prefix[32], pub[32], r[32], h[32];
    expand(a, prefix, seed);
    crypto_eddsa_scalarbase(pub, a);
    hash_reduce(r, prefix, 32, ctx, ctx_len, msg, len, "", 0);
    crypto_eddsa_scalarbase(sig, r);                 /* R = r*B */
    hash_reduce(h, sig, 32, pub, 32, ctx, ctx_len, msg, len);
    crypto_eddsa_mul_add(sig + 32, h, a, r);         /* S = h*a + r mod L */
    crypto_wipe(a, 32);
    crypto_wipe(prefix, 32);
    crypto_wipe(r, 32);
}
