/* SHA-256 dupa FIPS 180-4 (RFC 6234). Fara dependente de libc: nici memcpy/memset (bucle simple),
 * ca sa mearga la fel in kernel (-ffreestanding, fara SSE) si in userland. */
#include <crypto/sha256.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t st[8], const uint8_t *p){
    uint32_t w[64];
    for(int i = 0; i < 16; i++){
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for(int i = 16; i < 64; i++){
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for(int i = 0; i < 64; i++){
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    crypto_wipe(w, sizeof(w));
}

void sha256_init(sha256_ctx *c){
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for(int i = 0; i < 8; i++){
        c->h[i] = iv[i];
    }
    c->total = 0;
    c->fill = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len){
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while(len){
        if(c->fill == 0 && len >= SHA256_BLOCK_SIZE){
            block(c->h, p);
            p += SHA256_BLOCK_SIZE;
            len -= SHA256_BLOCK_SIZE;
            continue;
        }
        size_t n = SHA256_BLOCK_SIZE - c->fill;
        if(n > len){
            n = len;
        }
        for(size_t i = 0; i < n; i++){
            c->buf[c->fill + i] = p[i];
        }
        c->fill += (uint32_t)n;
        p += n;
        len -= n;
        if(c->fill == SHA256_BLOCK_SIZE){
            block(c->h, c->buf);
            c->fill = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]){
    uint64_t bits = c->total * 8;
    uint8_t pad[SHA256_BLOCK_SIZE + 8];
    uint32_t padlen = (c->fill < 56 ? 56 : 120) - c->fill;
    pad[0] = 0x80;
    for(uint32_t i = 1; i < padlen; i++){
        pad[i] = 0;
    }
    for(int i = 0; i < 8; i++){
        pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_update(c, pad, padlen + 8);
    for(int i = 0; i < 8; i++){
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
    crypto_wipe(c, sizeof(*c));
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]){
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void crypto_wipe(void *p, size_t len){
    volatile uint8_t *v = (volatile uint8_t *)p;
    while(len--){
        *v++ = 0;
    }
}

int crypto_ct_equal(const void *a, const void *b, size_t n){
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for(size_t i = 0; i < n; i++){
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0;
}
