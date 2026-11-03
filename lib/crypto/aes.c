#include <crypto/aes.h>
#include <crypto/sha256.h>      /* crypto_wipe */

typedef uint64_t u64;

static int force_soft;
static int hw_state = -1;       /* -1 = necunoscut */

int aes_hw_available(void){
#if defined(__x86_64__)
    if(hw_state < 0){
        uint32_t a = 1, b, c = 0, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        hw_state = (c >> 25) & 1;
    }
    return hw_state;
#else
    return 0;
#endif
}

void aes_force_software(int on){
    force_soft = on;
}

/* ---- aritmetica bitsliced: 8 plane, fiecare cu 64 de culoare (octeti de stare) ---- */

static void gf_reduce(u64 t[15]){
    /* x^8 = x^4 + x^3 + x + 1 */
    for(int k = 14; k >= 8; k--){
        u64 v = t[k];
        t[k - 4] ^= v;
        t[k - 5] ^= v;
        t[k - 7] ^= v;
        t[k - 8] ^= v;
    }
}

static void gf_mul(u64 r[8], const u64 a[8], const u64 b[8]){
    u64 t[15] = {0};
    for(int i = 0; i < 8; i++){
        for(int j = 0; j < 8; j++){
            t[i + j] ^= a[i] & b[j];
        }
    }
    gf_reduce(t);
    for(int i = 0; i < 8; i++){
        r[i] = t[i];
    }
}

static void gf_sq(u64 r[8], const u64 a[8]){
    u64 t[15] = {0};
    for(int i = 0; i < 8; i++){
        t[2 * i] = a[i];
    }
    gf_reduce(t);
    for(int i = 0; i < 8; i++){
        r[i] = t[i];
    }
}

/* x^254 = x^-1 (0 -> 0): 4 inmultiri, 8 patrate */
static void gf_inv(u64 x[8]){
    u64 x2[8], x3[8], x12[8], x15[8], t[8];
    gf_sq(x2, x);
    gf_mul(x3, x2, x);
    gf_sq(t, x3);
    gf_sq(x12, t);
    gf_mul(x15, x12, x3);
    gf_sq(t, x15);
    gf_sq(t, t);
    gf_sq(t, t);
    gf_sq(t, t);                /* x^240 */
    gf_mul(t, t, x12);          /* x^252 */
    gf_mul(x, t, x2);           /* x^254 */
}

static void bs_sub(u64 p[8]){
    gf_inv(p);
    u64 o[8];
    for(int i = 0; i < 8; i++){
        o[i] = p[i] ^ p[(i + 4) & 7] ^ p[(i + 5) & 7] ^ p[(i + 6) & 7] ^ p[(i + 7) & 7];
    }
    o[0] = ~o[0];
    o[1] = ~o[1];
    o[5] = ~o[5];
    o[6] = ~o[6];
    for(int i = 0; i < 8; i++){
        p[i] = o[i];
    }
}

static void bs_inv_sub(u64 p[8]){
    u64 y[8], o[8];
    for(int i = 0; i < 8; i++){
        y[i] = p[i];
    }
    y[0] = ~y[0];
    y[1] = ~y[1];
    y[5] = ~y[5];
    y[6] = ~y[6];
    for(int i = 0; i < 8; i++){
        o[i] = y[(i + 2) & 7] ^ y[(i + 5) & 7] ^ y[(i + 7) & 7];
    }
    gf_inv(o);
    for(int i = 0; i < 8; i++){
        p[i] = o[i];
    }
}

#define REP16(x) ((u64)(x) * 0x0001000100010001ULL)

static void bs_shift_rows(u64 p[8], int inv){
    for(int i = 0; i < 8; i++){
        u64 x = p[i], res = x & REP16(0x1111);          /* randul 0 ramane */
        for(int r = 1; r < 4; r++){
            u64 rowmask = REP16(0x1111u << r);
            int s = 4 * r;
            u64 v;
            if(!inv){
                u64 low = REP16((1u << (16 - s)) - 1);
                v = ((x >> s) & low) | ((x << (16 - s)) & ~low);
            } else {
                u64 hi = REP16((0xFFFFu << s) & 0xFFFFu);
                v = ((x << s) & hi) | ((x >> (16 - s)) & ~hi);
            }
            res |= v & rowmask;
        }
        p[i] = res;
    }
}

/* Valoarea de pe randul r+k a coloanei, adusa pe randul r (un nibble = o coloana, bitul = randul) */
static u64 rot_rows(u64 x, int k){
    switch(k){
    case 1: return ((x >> 1) & REP16(0x7777)) | ((x << 3) & REP16(0x8888));
    case 2: return ((x >> 2) & REP16(0x3333)) | ((x << 2) & REP16(0xCCCC));
    default: return ((x >> 3) & REP16(0x1111)) | ((x << 1) & REP16(0xEEEE));
    }
}

static void bs_xtime(u64 o[8], const u64 a[8]){
    o[0] = a[7];
    o[1] = a[0] ^ a[7];
    o[2] = a[1];
    o[3] = a[2] ^ a[7];
    o[4] = a[3] ^ a[7];
    o[5] = a[4];
    o[6] = a[5];
    o[7] = a[6];
}

static void bs_mix_columns(u64 p[8]){
    u64 r1[8], r2[8], r3[8], s[8], x[8];
    for(int i = 0; i < 8; i++){
        r1[i] = rot_rows(p[i], 1);
        r2[i] = rot_rows(p[i], 2);
        r3[i] = rot_rows(p[i], 3);
        s[i] = p[i] ^ r1[i];
    }
    bs_xtime(x, s);
    for(int i = 0; i < 8; i++){
        p[i] = x[i] ^ r1[i] ^ r2[i] ^ r3[i];
    }
}

static void bs_inv_mix_columns(u64 p[8]){
    u64 s[8], a[8], b[8];
    for(int i = 0; i < 8; i++){
        s[i] = p[i] ^ rot_rows(p[i], 2);
    }
    bs_xtime(a, s);
    bs_xtime(b, a);
    for(int i = 0; i < 8; i++){
        p[i] ^= b[i];
    }
    bs_mix_columns(p);
}

static void bs_add_key(u64 p[8], const uint16_t k[8]){
    for(int i = 0; i < 8; i++){
        p[i] ^= REP16(k[i]);
    }
}

/* n <= 4 blocuri -> plane (culoarul = bloc * 16 + octet) */
static void bs_pack(u64 p[8], const uint8_t *in, size_t n){
    for(int b = 0; b < 8; b++){
        p[b] = 0;
    }
    for(size_t i = 0; i < n * 16; i++){
        uint32_t v = in[i];
        for(int b = 0; b < 8; b++){
            p[b] |= (u64)((v >> b) & 1) << i;
        }
    }
}

static void bs_unpack(const u64 p[8], uint8_t *out, size_t n){
    for(size_t i = 0; i < n * 16; i++){
        uint32_t v = 0;
        for(int b = 0; b < 8; b++){
            v |= (uint32_t)((p[b] >> i) & 1) << b;
        }
        out[i] = (uint8_t)v;
    }
}

uint8_t aes_sbox_ct(uint8_t x){
    u64 p[8];
    for(int b = 0; b < 8; b++){
        p[b] = (u64)((x >> b) & 1);
    }
    bs_sub(p);
    uint32_t v = 0;
    for(int b = 0; b < 8; b++){
        v |= (uint32_t)(p[b] & 1) << b;
    }
    return (uint8_t)v;
}

static void sub_bytes_buf(uint8_t *b, size_t n){        /* n <= 64 */
    u64 p[8];
    for(int k = 0; k < 8; k++){
        p[k] = 0;
    }
    for(size_t i = 0; i < n; i++){
        for(int k = 0; k < 8; k++){
            p[k] |= (u64)((b[i] >> k) & 1) << i;
        }
    }
    bs_sub(p);
    for(size_t i = 0; i < n; i++){
        uint32_t v = 0;
        for(int k = 0; k < 8; k++){
            v |= (uint32_t)((p[k] >> i) & 1) << k;
        }
        b[i] = (uint8_t)v;
    }
}

static void bs_encrypt4(const aes_ctx *c, u64 p[8]){
    bs_add_key(p, c->bs[0]);
    for(int r = 1; r < c->rounds; r++){
        bs_sub(p);
        bs_shift_rows(p, 0);
        bs_mix_columns(p);
        bs_add_key(p, c->bs[r]);
    }
    bs_sub(p);
    bs_shift_rows(p, 0);
    bs_add_key(p, c->bs[c->rounds]);
}

static void bs_decrypt4(const aes_ctx *c, u64 p[8]){
    bs_add_key(p, c->bs[c->rounds]);
    for(int r = c->rounds - 1; r >= 1; r--){
        bs_shift_rows(p, 1);
        bs_inv_sub(p);
        bs_add_key(p, c->bs[r]);
        bs_inv_mix_columns(p);
    }
    bs_shift_rows(p, 1);
    bs_inv_sub(p);
    bs_add_key(p, c->bs[0]);
}

/* ---- AES-NI ---- */

#if defined(__x86_64__)
/* Kernelul e compilat fara SSE: xmm0/xmm1 se salveaza si se restaureaza aici, ca sa nu strice starea FPU a task-ului */
static void ni_block(const uint8_t *keys, int rounds, const uint8_t *in, uint8_t *out, int dec){
    uint8_t save[32];
    const uint8_t *kp = keys + 16;
    long cnt = rounds - 1;
    if(dec){
        __asm__ volatile(
            "movdqu %%xmm0, (%[sv])\n\t"
            "movdqu %%xmm1, 16(%[sv])\n\t"
            "movdqu (%[in]), %%xmm0\n\t"
            "movdqu (%[k]), %%xmm1\n\t"
            "pxor %%xmm1, %%xmm0\n\t"
            "1:\n\t"
            "movdqu (%[kp]), %%xmm1\n\t"
            "aesdec %%xmm1, %%xmm0\n\t"
            "add $16, %[kp]\n\t"
            "dec %[cnt]\n\t"
            "jnz 1b\n\t"
            "movdqu (%[kp]), %%xmm1\n\t"
            "aesdeclast %%xmm1, %%xmm0\n\t"
            "movdqu %%xmm0, (%[out])\n\t"
            "movdqu (%[sv]), %%xmm0\n\t"
            "movdqu 16(%[sv]), %%xmm1\n\t"
            : [kp] "+r"(kp), [cnt] "+r"(cnt)
            : [sv] "r"(save), [in] "r"(in), [out] "r"(out), [k] "r"(keys)
            : "memory", "cc");
    } else {
        __asm__ volatile(
            "movdqu %%xmm0, (%[sv])\n\t"
            "movdqu %%xmm1, 16(%[sv])\n\t"
            "movdqu (%[in]), %%xmm0\n\t"
            "movdqu (%[k]), %%xmm1\n\t"
            "pxor %%xmm1, %%xmm0\n\t"
            "1:\n\t"
            "movdqu (%[kp]), %%xmm1\n\t"
            "aesenc %%xmm1, %%xmm0\n\t"
            "add $16, %[kp]\n\t"
            "dec %[cnt]\n\t"
            "jnz 1b\n\t"
            "movdqu (%[kp]), %%xmm1\n\t"
            "aesenclast %%xmm1, %%xmm0\n\t"
            "movdqu %%xmm0, (%[out])\n\t"
            "movdqu (%[sv]), %%xmm0\n\t"
            "movdqu 16(%[sv]), %%xmm1\n\t"
            : [kp] "+r"(kp), [cnt] "+r"(cnt)
            : [sv] "r"(save), [in] "r"(in), [out] "r"(out), [k] "r"(keys)
            : "memory", "cc");
    }
}

static void ni_imc(const uint8_t *in, uint8_t *out){
    uint8_t save[16];
    __asm__ volatile(
        "movdqu %%xmm0, (%[sv])\n\t"
        "movdqu (%[in]), %%xmm0\n\t"
        "aesimc %%xmm0, %%xmm0\n\t"
        "movdqu %%xmm0, (%[out])\n\t"
        "movdqu (%[sv]), %%xmm0\n\t"
        :
        : [sv] "r"(save), [in] "r"(in), [out] "r"(out)
        : "memory");
}
#endif

/* ---- chei ---- */

int aes_set_key(aes_ctx *c, const uint8_t *key, size_t keylen){
    if(keylen != 16 && keylen != 24 && keylen != 32){
        return -1;
    }
    int nk = (int)(keylen / 4);
    c->rounds = nk + 6;
    int total = 4 * (c->rounds + 1);
    for(size_t i = 0; i < keylen; i++){
        c->rk[i] = key[i];
    }
    uint8_t rcon = 1;
    for(int i = nk; i < total; i++){
        uint8_t t[4];
        for(int k = 0; k < 4; k++){
            t[k] = c->rk[4 * (i - 1) + k];
        }
        if(i % nk == 0){
            uint8_t r0 = t[0];
            t[0] = t[1];
            t[1] = t[2];
            t[2] = t[3];
            t[3] = r0;
            sub_bytes_buf(t, 4);
            t[0] ^= rcon;
            rcon = (uint8_t)((rcon << 1) ^ (0x1B & (0 - (rcon >> 7))));
        } else if(nk > 6 && i % nk == 4){
            sub_bytes_buf(t, 4);
        }
        for(int k = 0; k < 4; k++){
            c->rk[4 * i + k] = (uint8_t)(c->rk[4 * (i - nk) + k] ^ t[k]);
        }
    }
    for(int r = 0; r <= c->rounds; r++){
        for(int b = 0; b < 8; b++){
            uint16_t v = 0;
            for(int i = 0; i < 16; i++){
                v |= (uint16_t)(((c->rk[16 * r + i] >> b) & 1) << i);
            }
            c->bs[r][b] = v;
        }
    }
    c->hw = 0;
#if defined(__x86_64__)
    if(!force_soft && aes_hw_available()){
        c->hw = 1;
        for(int j = 0; j <= c->rounds; j++){
            if(j == 0){
                for(int i = 0; i < 16; i++){
                    c->dk[i] = c->rk[16 * c->rounds + i];
                }
            } else if(j == c->rounds){
                for(int i = 0; i < 16; i++){
                    c->dk[16 * j + i] = c->rk[i];
                }
            } else {
                ni_imc(c->rk + 16 * (c->rounds - j), c->dk + 16 * j);
            }
        }
    }
#endif
    return 0;
}

void aes_encrypt_blocks(const aes_ctx *c, const uint8_t *in, uint8_t *out, size_t nblocks){
#if defined(__x86_64__)
    if(c->hw){
        for(size_t i = 0; i < nblocks; i++){
            ni_block(c->rk, c->rounds, in + 16 * i, out + 16 * i, 0);
        }
        return;
    }
#endif
    while(nblocks){
        size_t n = nblocks < 4 ? nblocks : 4;
        u64 p[8];
        bs_pack(p, in, n);
        bs_encrypt4(c, p);
        bs_unpack(p, out, n);
        in += 16 * n;
        out += 16 * n;
        nblocks -= n;
    }
}

void aes_decrypt_blocks(const aes_ctx *c, const uint8_t *in, uint8_t *out, size_t nblocks){
#if defined(__x86_64__)
    if(c->hw){
        for(size_t i = 0; i < nblocks; i++){
            ni_block(c->dk, c->rounds, in + 16 * i, out + 16 * i, 1);
        }
        return;
    }
#endif
    while(nblocks){
        size_t n = nblocks < 4 ? nblocks : 4;
        u64 p[8];
        bs_pack(p, in, n);
        bs_decrypt4(c, p);
        bs_unpack(p, out, n);
        in += 16 * n;
        out += 16 * n;
        nblocks -= n;
    }
}

void aes_wipe(aes_ctx *c){
    crypto_wipe(c, sizeof(*c));
}
