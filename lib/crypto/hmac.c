#include <crypto/hmac.h>

void hmac_sha256_init(hmac_sha256_ctx *c, const void *key, size_t keylen){
    uint8_t k[SHA256_BLOCK_SIZE];
    for(int i = 0; i < SHA256_BLOCK_SIZE; i++){
        k[i] = 0;
    }
    if(keylen > SHA256_BLOCK_SIZE){
        sha256(key, keylen, k);
    } else {
        const uint8_t *kp = (const uint8_t *)key;
        for(size_t i = 0; i < keylen; i++){
            k[i] = kp[i];
        }
    }
    uint8_t pad[SHA256_BLOCK_SIZE];
    for(int i = 0; i < SHA256_BLOCK_SIZE; i++){
        pad[i] = k[i] ^ 0x36;
    }
    sha256_init(&c->inner);
    sha256_update(&c->inner, pad, sizeof(pad));
    for(int i = 0; i < SHA256_BLOCK_SIZE; i++){
        pad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&c->outer);
    sha256_update(&c->outer, pad, sizeof(pad));
    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
}

void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len){
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]){
    uint8_t ih[SHA256_DIGEST_SIZE];
    sha256_final(&c->inner, ih);
    sha256_update(&c->outer, ih, sizeof(ih));
    sha256_final(&c->outer, out);
    crypto_wipe(ih, sizeof(ih));
}

void hmac_sha256(const void *key, size_t keylen, const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]){
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, keylen);
    hmac_sha256_update(&c, data, len);
    hmac_sha256_final(&c, out);
}
