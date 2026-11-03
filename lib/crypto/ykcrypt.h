#ifndef YOKAI_CRYPTO_YKCRYPT_H
#define YOKAI_CRYPTO_YKCRYPT_H
/* Formatul antetului unui volum criptat YokaiOS ("YKCRYPT1"), folosit de kernel (kernel/block/crypt.cpp), de
 * /sbin/cryptsetup si de tools/cryptimg.c (gazda). Antetul ocupa primii 4096 de octeti ai dispozitivului; datele
 * criptate incep la payload_offset (4096). Campurile sunt little-endian:
 *    0  magic "YKCRYPT1"          8  versiune (1)             12  cifru (YKC_AES_XTS / YKC_CHACHA20)
 *   16  iteratii PBKDF2          20  sector (512)             24  payload_offset (u64)
 *   32  payload_bytes (u64)      40  uuid[16]                 56  salt[32]
 *   88  nonce[12] (impachetare)  100 cheie master impachetata[64]   164 mac[32]   (restul pana la 4096 = 0)
 * Cheia de deschidere: kek = PBKDF2-HMAC-SHA256(parola, salt, iteratii, 32); kek_enc = HMAC(kek, "yokai-crypt-enc"),
 * kek_mac = HMAC(kek, "yokai-crypt-mac"). Cheia master (64 de octeti aleatori) se impacheteaza cu ChaCha20(kek_enc,
 * nonce) si se autentifica cu HMAC-SHA256(kek_mac, antet[0..164)): parola gresita sau antet modificat = mac diferit.
 * Cheia master nu se scrie niciodata necriptata. Un singur keyslot. */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YKC_HDR_SIZE      4096
#define YKC_MAGIC         "YKCRYPT1"
#define YKC_VERSION       1
#define YKC_AES_XTS       1         /* AES-256-XTS, tweak plain64; cheia master = 64 de octeti */
#define YKC_CHACHA20      2         /* ChaCha20 pe sector (cheie = primii 32 de octeti); cifru de flux, FARA autentificare */
#define YKC_MASTER_LEN    64
#define YKC_SECTOR        512
#define YKC_ITER_MIN      1000
#define YKC_ITER_MAX      10000000
#define YKC_ITER_DEFAULT  100000
/* Octeti aleatori ceruti la formatare: salt 32 + nonce 12 + uuid 16 + cheia master 64 */
#define YKC_RND_LEN       124

struct ykc_info {
    uint32_t cipher;
    uint32_t iterations;
    uint32_t sector;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint8_t  uuid[16];
};

/* Completeaza hdr (YKC_HDR_SIZE octeti) pentru un volum de total_bytes (antet + date); rnd = YKC_RND_LEN octeti
 * aleatori dati de apelant; master_out (YKC_MASTER_LEN, poate fi NULL) primeste cheia master. 0 sau -1 la argumente
 * invalide (cifru necunoscut, iteratii in afara [YKC_ITER_MIN, YKC_ITER_MAX], volum prea mic) */
int ykc_format(uint8_t *hdr, const void *pw, size_t pwlen, uint32_t cipher, uint32_t iter, uint64_t total_bytes,
               const uint8_t *rnd, uint8_t *master_out);
/* Valideaza structura antetului (fara parola): 0 sau -1 */
int ykc_parse(const uint8_t *hdr, struct ykc_info *out);
/* Deschide cu parola: 0 si cheia master in master (YKC_MASTER_LEN), -1 antet invalid, -2 parola gresita / antet modificat */
int ykc_unlock(const uint8_t *hdr, const void *pw, size_t pwlen, uint8_t *master);
/* Reimpacheteaza aceeasi cheie master sub o parola noua (alt salt/nonce din rnd = 44 de octeti: salt 32 + nonce 12);
 * 0, -1 antet invalid, -2 parola veche gresita */
int ykc_rewrap(uint8_t *hdr, const void *oldpw, size_t oldlen, const void *newpw, size_t newlen, uint32_t iter,
               const uint8_t *rnd);

#ifdef __cplusplus
}
#endif
#endif
