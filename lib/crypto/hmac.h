#ifndef YOKAI_CRYPTO_HMAC_H
#define YOKAI_CRYPTO_HMAC_H
/* HMAC-SHA256 (RFC 2104 / RFC 4231) */
#include <crypto/sha256.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sha256_ctx inner;   /* dupa H(K ^ ipad) */
    sha256_ctx outer;   /* dupa H(K ^ opad) */
} hmac_sha256_ctx;

/* Cheia poate avea orice lungime (peste 64 de octeti se hash-uieste intai) */
void hmac_sha256_init(hmac_sha256_ctx *c, const void *key, size_t keylen);
void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);   /* sterge contextul */
void hmac_sha256(const void *key, size_t keylen, const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif
#endif
