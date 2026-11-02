#ifndef YOKAI_CRYPTO_PBKDF2_H
#define YOKAI_CRYPTO_PBKDF2_H
/* PBKDF2 cu HMAC-SHA256 (RFC 8018 / RFC 7914 sectiunea 11) */
#include <crypto/hmac.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deriva outlen octeti din parola (pw, pwlen) si salt. iter >= 1. Intoarce 0, sau -1 la argumente invalide
 * (iter == 0, salt/out NULL, outlen peste (2^32 - 1) * 32) */
int pbkdf2_hmac_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen,
                       uint32_t iter, uint8_t *out, size_t outlen);

#ifdef __cplusplus
}
#endif
#endif
