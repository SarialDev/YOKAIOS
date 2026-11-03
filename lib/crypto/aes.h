#ifndef YOKAI_CRYPTO_AES_H
#define YOKAI_CRYPTO_AES_H
/* AES-128/192/256 (FIPS 197), numai blocuri independente (ECB); modurile (XTS) sunt in xts.h.
 * Doua implementari cu acelasi rezultat:
 *  - AES-NI (aesenc/aesdec) cand CPU-ul o are: instructiunile sunt in timp constant;
 *  - software "bitsliced", in timp constant: fara tabele si fara ramificatii dependente de date. Starea a 4 blocuri
 *    sta in 8 cuvinte de 64 de biti (un cuvant = un bit al fiecarui octet de stare); S-box-ul e inversul in
 *    GF(2^8) calculat aritmetic (x^254) + transformarea afina, deci nu exista acces la memorie indexat de secret.
 * Cod C freestanding folosit de kernel, de libc.a si de uneltele de pe gazda (tools/cryptimg.c). */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES_BLOCK_SIZE 16
#define AES_MAX_ROUNDS 14

typedef struct {
    uint16_t bs[AES_MAX_ROUNDS + 1][8];         /* cheile de runda in forma bitsliced (software) */
    uint8_t  rk[(AES_MAX_ROUNDS + 1) * 16];     /* cheile de runda pentru aesenc */
    uint8_t  dk[(AES_MAX_ROUNDS + 1) * 16];     /* cheile pentru aesdec (aesimc), numai cu hw */
    int      rounds;
    int      hw;                                /* 1 = AES-NI pentru aceasta cheie */
} aes_ctx;

/* CPU-ul are AES-NI? */
int aes_hw_available(void);
/* on = 1: cheile setate de acum inainte folosesc doar varianta software (pentru teste si comparatii) */
void aes_force_software(int on);
/* keylen = 16, 24 sau 32; 0 sau -1 (lungime gresita) */
int aes_set_key(aes_ctx *c, const uint8_t *key, size_t keylen);
/* nblocks blocuri de 16 octeti; in si out pot fi acelasi buffer */
void aes_encrypt_blocks(const aes_ctx *c, const uint8_t *in, uint8_t *out, size_t nblocks);
void aes_decrypt_blocks(const aes_ctx *c, const uint8_t *in, uint8_t *out, size_t nblocks);
/* S-box-ul calculat in timp constant (pentru testul exhaustiv fata de tabela standard) */
uint8_t aes_sbox_ct(uint8_t x);
void aes_wipe(aes_ctx *c);

#ifdef __cplusplus
}
#endif
#endif
