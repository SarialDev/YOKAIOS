#include <crypto/pbkdf2.h>

int pbkdf2_hmac_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen,
                       uint32_t iter, uint8_t *out, size_t outlen){
    if(iter == 0 || !out || (!salt && saltlen) || (!pw && pwlen) || outlen / SHA256_DIGEST_SIZE >= 0xffffffffu){
        return -1;
    }
    hmac_sha256_ctx keyed;                  /* cheia HMAC se pregateste o singura data */
    hmac_sha256_init(&keyed, pw, pwlen);
    for(uint32_t blk = 1; outlen; blk++){
        uint8_t be[4] = {(uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk};
        uint8_t u[SHA256_DIGEST_SIZE], t[SHA256_DIGEST_SIZE];
        hmac_sha256_ctx h = keyed;
        hmac_sha256_update(&h, salt, saltlen);
        hmac_sha256_update(&h, be, 4);
        hmac_sha256_final(&h, u);
        for(int i = 0; i < SHA256_DIGEST_SIZE; i++){
            t[i] = u[i];
        }
        for(uint32_t j = 1; j < iter; j++){
            h = keyed;
            hmac_sha256_update(&h, u, sizeof(u));
            hmac_sha256_final(&h, u);
            for(int i = 0; i < SHA256_DIGEST_SIZE; i++){
                t[i] ^= u[i];
            }
        }
        size_t n = outlen < SHA256_DIGEST_SIZE ? outlen : SHA256_DIGEST_SIZE;
        for(size_t i = 0; i < n; i++){
            out[i] = t[i];
        }
        out += n;
        outlen -= n;
        crypto_wipe(u, sizeof(u));
        crypto_wipe(t, sizeof(t));
    }
    crypto_wipe(&keyed, sizeof(keyed));
    return 0;
}
