#ifndef YOKAI_CRYPTO_PWHASH_H
#define YOKAI_CRYPTO_PWHASH_H
/* Formatul hash-urilor din /etc/shadow (si /etc/auth): "$pbkdf2-sha256$<iteratii>$<salt hex, 32>$<hash hex, 64>"
 * (salt de 16 octeti, hash de 32). Folosit pentru parole, PIN-uri si fisiere-cheie. Cod freestanding. */
#include <crypto/pbkdf2.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWHASH_PREFIX      "$pbkdf2-sha256$"
#define PWHASH_SALT_LEN    16
#define PWHASH_HASH_LEN    32
#define PWHASH_ITER_MIN    100000u      /* minim pentru hash-uri noi */
#define PWHASH_ITER_DEFAULT 100000u
#define PWHASH_ITER_MAX    10000000u    /* verify refuza mai mult (altfel un fisier stricat blocheaza login-ul) */
#define PWHASH_MAX         128          /* octeti, cu \0 final */

/* Scrie in out (cap >= PWHASH_MAX) hash-ul secretului dat cu saltul salt[16] si iter iteratii.
 * 0 la succes, -1 daca iter < PWHASH_ITER_MIN sau > PWHASH_ITER_MAX sau cap prea mic */
int pwhash_make(const void *secret, size_t len, const uint8_t salt[PWHASH_SALT_LEN], uint32_t iter,
                char *out, size_t cap);

/* 1 = secretul corespunde hash-ului stored, 0 = nu, -1 = stored nu e un hash valid.
 * Compararea se face in timp constant. Acceptă orice iteratii intre 1 si PWHASH_ITER_MAX (verificare) */
int pwhash_verify(const void *secret, size_t len, const char *stored);

/* stored arata ca un hash complet (formatul de mai sus)? */
int pwhash_valid(const char *stored);

#ifdef __cplusplus
}
#endif
#endif
