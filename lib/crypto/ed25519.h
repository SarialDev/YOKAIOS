#ifndef YOKAI_CRYPTO_ED25519_H
#define YOKAI_CRYPTO_ED25519_H
/* Verificare Ed25519 (RFC 8032, SHA-512) peste Monocypher (third_party/monocypher). Doar verificarea: cheile private si
 * semnarea traiesc pe gazda, in tools/sign.py. Kernel si userland (libc.a). */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ED25519_PUBKEY_SIZE 32
#define ED25519_SIG_SIZE    64

/* Mesajul semnat este ctx || msg (ctx = prefix de separare a domeniilor: "yokai-pkg-v1\n" etc.; ctx_len 0 = Ed25519 simplu).
 * 0 = semnatura valida, -1 = invalida (inclusiv S >= L sau cheie / R care nu sunt pe curba) */
int ed25519_verify(const uint8_t sig[ED25519_SIG_SIZE], const uint8_t pub[ED25519_PUBKEY_SIZE],
                   const void *ctx, size_t ctx_len, const void *msg, size_t len);
/* Pentru teste: cheia publica dintr-un seed de 32 de octeti si semnarea (RFC 8032 sectiunea 5.1) */
void ed25519_public_key(uint8_t pub[ED25519_PUBKEY_SIZE], const uint8_t seed[32]);
void ed25519_sign(uint8_t sig[ED25519_SIG_SIZE], const uint8_t seed[32], const void *ctx, size_t ctx_len,
                  const void *msg, size_t len);

#ifdef __cplusplus
}
#endif
#endif
