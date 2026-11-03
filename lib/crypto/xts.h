#ifndef YOKAI_CRYPTO_XTS_H
#define YOKAI_CRYPTO_XTS_H
/* AES-XTS (IEEE 1619-2007, NIST SP 800-38E) pentru discuri. Cheia = doua chei AES concatenate (32 octeti = XTS-AES-128,
 * 64 octeti = XTS-AES-256). Tweak-ul unei unitati de date = numarul ei de sector pe 128 de biti little-endian
 * ("plain64" din dm-crypt). Unitatea are un numar intreg de blocuri de 16 octeti (fara ciphertext stealing). */
#include <crypto/aes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    aes_ctx data;       /* K1: cripteaza datele */
    aes_ctx tweak;      /* K2: cripteaza tweak-ul */
} xts_ctx;

/* keylen = 32 sau 64; cheile K1 si K2 trebuie sa difere (altfel -1, ca in standard) */
int xts_set_key(xts_ctx *c, const uint8_t *key, size_t keylen);
/* len = multiplu de 16, >= 16; in si out pot fi acelasi buffer. 0 sau -1 */
int xts_encrypt(const xts_ctx *c, uint64_t sector, const uint8_t *in, uint8_t *out, size_t len);
int xts_decrypt(const xts_ctx *c, uint64_t sector, const uint8_t *in, uint8_t *out, size_t len);
void xts_wipe(xts_ctx *c);

#ifdef __cplusplus
}
#endif
#endif
