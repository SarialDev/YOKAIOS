#include <crypto/xts.h>
#include <crypto/sha256.h>

int xts_set_key(xts_ctx *c, const uint8_t *key, size_t keylen){
    if(keylen != 32 && keylen != 64){
        return -1;
    }
    size_t h = keylen / 2;
    if(crypto_ct_equal(key, key + h, h)){
        return -1;
    }
    if(aes_set_key(&c->data, key, h) || aes_set_key(&c->tweak, key + h, h)){
        return -1;
    }
    return 0;
}

/* T *= x in GF(2^128) (polinomul x^128 + x^7 + x^2 + x + 1), reprezentare little-endian, fara ramificari */
static void tweak_mul(uint8_t t[16]){
    uint8_t carry = (uint8_t)(t[15] >> 7);
    for(int i = 15; i > 0; i--){
        t[i] = (uint8_t)((t[i] << 1) | (t[i - 1] >> 7));
    }
    t[0] = (uint8_t)((t[0] << 1) ^ (0x87 & (0 - carry)));
}

static int xts_crypt(const xts_ctx *c, int dec, uint64_t sector, const uint8_t *in, uint8_t *out, size_t len){
    if(len < 16 || (len & 15)){
        return -1;
    }
    uint8_t t[16] = {0};
    for(int i = 0; i < 8; i++){
        t[i] = (uint8_t)(sector >> (8 * i));
    }
    aes_encrypt_blocks(&c->tweak, t, t, 1);
    uint8_t tw[64], x[64];
    size_t nblocks = len / 16;
    while(nblocks){
        size_t n = nblocks < 4 ? nblocks : 4;
        for(size_t j = 0; j < n; j++){
            for(int k = 0; k < 16; k++){
                tw[16 * j + k] = t[k];
                x[16 * j + k] = (uint8_t)(in[16 * j + k] ^ t[k]);
            }
            tweak_mul(t);
        }
        if(dec){
            aes_decrypt_blocks(&c->data, x, x, n);
        } else {
            aes_encrypt_blocks(&c->data, x, x, n);
        }
        for(size_t k = 0; k < 16 * n; k++){
            out[k] = (uint8_t)(x[k] ^ tw[k]);
        }
        in += 16 * n;
        out += 16 * n;
        nblocks -= n;
    }
    crypto_wipe(t, sizeof(t));
    crypto_wipe(tw, sizeof(tw));
    crypto_wipe(x, sizeof(x));
    return 0;
}

int xts_encrypt(const xts_ctx *c, uint64_t sector, const uint8_t *in, uint8_t *out, size_t len){
    return xts_crypt(c, 0, sector, in, out, len);
}

int xts_decrypt(const xts_ctx *c, uint64_t sector, const uint8_t *in, uint8_t *out, size_t len){
    return xts_crypt(c, 1, sector, in, out, len);
}

void xts_wipe(xts_ctx *c){
    crypto_wipe(c, sizeof(*c));
}
