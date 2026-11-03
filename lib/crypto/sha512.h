#ifndef YOKAI_CRYPTO_SHA512_H
#define YOKAI_CRYPTO_SHA512_H
/* SHA-512 (FIPS 180-4 / RFC 6234), necesar pentru Ed25519 (RFC 8032). Cod C freestanding: kernel si userland (libc.a). */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA512_BLOCK_SIZE  128
#define SHA512_DIGEST_SIZE 64

typedef struct {
    uint64_t h[8];
    uint64_t total;                     /* octeti procesati (mesajele > 2^61 octeti nu se suporta) */
    uint8_t  buf[SHA512_BLOCK_SIZE];
    uint32_t fill;
} sha512_ctx;

void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t len);
void sha512_final(sha512_ctx *c, uint8_t out[SHA512_DIGEST_SIZE]);   /* sterge si contextul */
void sha512(const void *data, size_t len, uint8_t out[SHA512_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif
#endif
