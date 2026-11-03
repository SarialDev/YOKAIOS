#ifndef YOKAI_CRYPTO_CHACHA20_H
#define YOKAI_CRYPTO_CHACHA20_H
/* ChaCha20 (RFC 8439): cheie de 256 de biti, nonce de 96, contor de 32. Foloseste doar adunari, rotiri si XOR (timp
 * constant, fara tabele). Pe disc e un cifru de flux: vezi kernel/block/crypt.h pentru limitele lui fata de XTS. */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Un bloc de 64 de octeti de flux de chei */
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
/* out = in XOR flux (cripteaza si decripteaza); in si out pot fi acelasi buffer */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                  const uint8_t *in, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
#endif
