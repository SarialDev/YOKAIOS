#ifndef YOKAI_CRYPTO_SHA256_H
#define YOKAI_CRYPTO_SHA256_H
/* SHA-256 (FIPS 180-4 / RFC 6234). Cod C freestanding: il folosesc si kernelul, si userland-ul (libc.a). */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t h[8];
    uint64_t total;                     /* octeti procesati pana acum */
    uint8_t  buf[SHA256_BLOCK_SIZE];
    uint32_t fill;                      /* octeti in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);   /* sterge si contextul */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* Sterge memoria in mod garantat (compilatorul nu o elimina) */
void crypto_wipe(void *p, size_t len);
/* Compara a si b (n octeti) in timp constant: 1 daca sunt egale, 0 altfel */
int crypto_ct_equal(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif
#endif
