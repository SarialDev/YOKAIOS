#include <crypto/ykcrypt.h>
#include <crypto/hmac.h>
#include <crypto/pbkdf2.h>
#include <crypto/chacha20.h>

#define OFF_MAGIC   0
#define OFF_VERSION 8
#define OFF_CIPHER  12
#define OFF_ITER    16
#define OFF_SECTOR  20
#define OFF_POFF    24
#define OFF_PBYTES  32
#define OFF_UUID    40
#define OFF_SALT    56
#define OFF_NONCE   88
#define OFF_WRAPPED 100
#define OFF_MAC     164
#define MAC_COVERS  164

static void st32(uint8_t *p, uint32_t v){
    for(int i = 0; i < 4; i++){
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void st64(uint8_t *p, uint64_t v){
    for(int i = 0; i < 8; i++){
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t ld32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t ld64(const uint8_t *p){
    return (uint64_t)ld32(p) | (uint64_t)ld32(p + 4) << 32;
}

static void cp(uint8_t *d, const uint8_t *s, size_t n){
    for(size_t i = 0; i < n; i++){
        d[i] = s[i];
    }
}

static int derive(const void *pw, size_t pwlen, const uint8_t *salt, uint32_t iter, uint8_t enc[32], uint8_t mac[32]){
    uint8_t kek[32];
    if(pbkdf2_hmac_sha256(pw, pwlen, salt, 32, iter, kek, 32)){
        return -1;
    }
    hmac_sha256(kek, 32, "yokai-crypt-enc", 15, enc);
    hmac_sha256(kek, 32, "yokai-crypt-mac", 15, mac);
    crypto_wipe(kek, sizeof(kek));
    return 0;
}

static void seal(uint8_t *hdr, const uint8_t *enc, const uint8_t *mac, const uint8_t *master){
    chacha20_xor(enc, 0, hdr + OFF_NONCE, master, hdr + OFF_WRAPPED, YKC_MASTER_LEN);
    hmac_sha256(mac, 32, hdr, MAC_COVERS, hdr + OFF_MAC);
}

int ykc_format(uint8_t *hdr, const void *pw, size_t pwlen, uint32_t cipher, uint32_t iter, uint64_t total_bytes,
               const uint8_t *rnd, uint8_t *master_out){
    if((cipher != YKC_AES_XTS && cipher != YKC_CHACHA20) || iter < YKC_ITER_MIN || iter > YKC_ITER_MAX ||
       total_bytes < YKC_HDR_SIZE + 2 * YKC_SECTOR){
        return -1;
    }
    for(size_t i = 0; i < YKC_HDR_SIZE; i++){
        hdr[i] = 0;
    }
    cp(hdr + OFF_MAGIC, (const uint8_t *)YKC_MAGIC, 8);
    st32(hdr + OFF_VERSION, YKC_VERSION);
    st32(hdr + OFF_CIPHER, cipher);
    st32(hdr + OFF_ITER, iter);
    st32(hdr + OFF_SECTOR, YKC_SECTOR);
    st64(hdr + OFF_POFF, YKC_HDR_SIZE);
    st64(hdr + OFF_PBYTES, (total_bytes - YKC_HDR_SIZE) & ~(uint64_t)(YKC_SECTOR - 1));
    cp(hdr + OFF_SALT, rnd, 32);
    cp(hdr + OFF_NONCE, rnd + 32, 12);
    cp(hdr + OFF_UUID, rnd + 44, 16);
    uint8_t master[YKC_MASTER_LEN], enc[32], mac[32];
    cp(master, rnd + 60, YKC_MASTER_LEN);
    int rc = derive(pw, pwlen, hdr + OFF_SALT, iter, enc, mac);
    if(!rc){
        seal(hdr, enc, mac, master);
        if(master_out){
            cp(master_out, master, YKC_MASTER_LEN);
        }
    }
    crypto_wipe(master, sizeof(master));
    crypto_wipe(enc, sizeof(enc));
    crypto_wipe(mac, sizeof(mac));
    return rc;
}

int ykc_parse(const uint8_t *hdr, struct ykc_info *out){
    for(int i = 0; i < 8; i++){
        if(hdr[OFF_MAGIC + i] != (uint8_t)YKC_MAGIC[i]){
            return -1;
        }
    }
    uint32_t cipher = ld32(hdr + OFF_CIPHER), iter = ld32(hdr + OFF_ITER);
    if(ld32(hdr + OFF_VERSION) != YKC_VERSION || (cipher != YKC_AES_XTS && cipher != YKC_CHACHA20) ||
       iter < YKC_ITER_MIN || iter > YKC_ITER_MAX || ld32(hdr + OFF_SECTOR) != YKC_SECTOR ||
       ld64(hdr + OFF_POFF) != YKC_HDR_SIZE || ld64(hdr + OFF_PBYTES) < 2 * YKC_SECTOR ||
       (ld64(hdr + OFF_PBYTES) % YKC_SECTOR)){
        return -1;
    }
    if(out){
        out->cipher = cipher;
        out->iterations = iter;
        out->sector = YKC_SECTOR;
        out->payload_offset = ld64(hdr + OFF_POFF);
        out->payload_bytes = ld64(hdr + OFF_PBYTES);
        cp(out->uuid, hdr + OFF_UUID, 16);
    }
    return 0;
}

int ykc_unlock(const uint8_t *hdr, const void *pw, size_t pwlen, uint8_t *master){
    struct ykc_info in;
    if(ykc_parse(hdr, &in)){
        return -1;
    }
    uint8_t enc[32], mac[32], want[32];
    if(derive(pw, pwlen, hdr + OFF_SALT, in.iterations, enc, mac)){
        return -1;
    }
    hmac_sha256(mac, 32, hdr, MAC_COVERS, want);
    int ok = crypto_ct_equal(want, hdr + OFF_MAC, 32);
    if(ok){
        chacha20_xor(enc, 0, hdr + OFF_NONCE, hdr + OFF_WRAPPED, master, YKC_MASTER_LEN);
    }
    crypto_wipe(enc, sizeof(enc));
    crypto_wipe(mac, sizeof(mac));
    crypto_wipe(want, sizeof(want));
    return ok ? 0 : -2;
}

int ykc_rewrap(uint8_t *hdr, const void *oldpw, size_t oldlen, const void *newpw, size_t newlen, uint32_t iter,
               const uint8_t *rnd){
    uint8_t master[YKC_MASTER_LEN], enc[32], mac[32];
    if(iter < YKC_ITER_MIN || iter > YKC_ITER_MAX){
        return -1;
    }
    int rc = ykc_unlock(hdr, oldpw, oldlen, master);
    if(!rc){
        cp(hdr + OFF_SALT, rnd, 32);
        cp(hdr + OFF_NONCE, rnd + 32, 12);
        st32(hdr + OFF_ITER, iter);
        rc = derive(newpw, newlen, hdr + OFF_SALT, iter, enc, mac);
        if(!rc){
            seal(hdr, enc, mac, master);
        }
    }
    crypto_wipe(master, sizeof(master));
    crypto_wipe(enc, sizeof(enc));
    crypto_wipe(mac, sizeof(mac));
    return rc;
}
