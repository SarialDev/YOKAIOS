#include <crypto/chacha20.h>
#include <crypto/sha256.h>

static uint32_t rotl(uint32_t x, int n){
    return (x << n) | (x >> (32 - n));
}

static uint32_t ld32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl(d, 16); \
    c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8); \
    c += d; b ^= c; b = rotl(b, 7)

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]){
    uint32_t s[16], x[16];
    s[0] = 0x61707865;
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for(int i = 0; i < 8; i++){
        s[4 + i] = ld32(key + 4 * i);
    }
    s[12] = counter;
    for(int i = 0; i < 3; i++){
        s[13 + i] = ld32(nonce + 4 * i);
    }
    for(int i = 0; i < 16; i++){
        x[i] = s[i];
    }
    for(int r = 0; r < 10; r++){
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for(int i = 0; i < 16; i++){
        uint32_t v = x[i] + s[i];
        out[4 * i] = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
    crypto_wipe(x, sizeof(x));
    crypto_wipe(s, sizeof(s));
}

void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                  const uint8_t *in, uint8_t *out, size_t len){
    uint8_t ks[64];
    while(len){
        size_t n = len < 64 ? len : 64;
        chacha20_block(key, counter++, nonce, ks);
        for(size_t i = 0; i < n; i++){
            out[i] = (uint8_t)(in[i] ^ ks[i]);
        }
        in += n;
        out += n;
        len -= n;
    }
    crypto_wipe(ks, sizeof(ks));
}
