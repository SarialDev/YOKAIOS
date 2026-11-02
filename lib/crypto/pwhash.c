#include <crypto/pwhash.h>

static const char HEXD[] = "0123456789abcdef";

static size_t slen(const char *s){
    size_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

static int hexval(char c){
    if(c >= '0' && c <= '9'){
        return c - '0';
    }
    if(c >= 'a' && c <= 'f'){
        return c - 'a' + 10;
    }
    if(c >= 'A' && c <= 'F'){
        return c - 'A' + 10;
    }
    return -1;
}

/* n octeti din 2n cifre hex; 0 la succes */
static int unhex(const char *s, uint8_t *out, size_t n){
    for(size_t i = 0; i < n; i++){
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if(hi < 0 || lo < 0){
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static char *put_hex(char *p, const uint8_t *b, size_t n){
    for(size_t i = 0; i < n; i++){
        *p++ = HEXD[b[i] >> 4];
        *p++ = HEXD[b[i] & 15];
    }
    return p;
}

/* Imparte stored in iteratii, salt (16 octeti), hash (32 octeti); 0 la succes */
static int parse(const char *s, uint32_t *iter, uint8_t salt[PWHASH_SALT_LEN], uint8_t hash[PWHASH_HASH_LEN]){
    const char *pre = PWHASH_PREFIX;
    size_t i = 0;
    while(pre[i]){
        if(s[i] != pre[i]){
            return -1;
        }
        i++;
    }
    s += i;
    uint64_t it = 0;
    size_t nd = 0;
    while(s[nd] >= '0' && s[nd] <= '9'){
        it = it * 10 + (uint64_t)(s[nd] - '0');
        if(++nd > 9){
            return -1;
        }
    }
    if(nd == 0 || it == 0 || it > PWHASH_ITER_MAX || s[nd] != '$'){
        return -1;
    }
    s += nd + 1;
    if(slen(s) != 2 * PWHASH_SALT_LEN + 1 + 2 * PWHASH_HASH_LEN || s[2 * PWHASH_SALT_LEN] != '$'){
        return -1;
    }
    if(unhex(s, salt, PWHASH_SALT_LEN) < 0 || unhex(s + 2 * PWHASH_SALT_LEN + 1, hash, PWHASH_HASH_LEN) < 0){
        return -1;
    }
    *iter = (uint32_t)it;
    return 0;
}

int pwhash_make(const void *secret, size_t len, const uint8_t salt[PWHASH_SALT_LEN], uint32_t iter,
                char *out, size_t cap){
    if(iter < PWHASH_ITER_MIN || iter > PWHASH_ITER_MAX || cap < PWHASH_MAX){
        return -1;
    }
    uint8_t h[PWHASH_HASH_LEN];
    if(pbkdf2_hmac_sha256(secret, len, salt, PWHASH_SALT_LEN, iter, h, sizeof(h)) < 0){
        return -1;
    }
    char *p = out;
    for(const char *pre = PWHASH_PREFIX; *pre; ){
        *p++ = *pre++;
    }
    char dig[10];
    int nd = 0;
    for(uint32_t v = iter; v; v /= 10){
        dig[nd++] = (char)('0' + v % 10);
    }
    while(nd){
        *p++ = dig[--nd];
    }
    *p++ = '$';
    p = put_hex(p, salt, PWHASH_SALT_LEN);
    *p++ = '$';
    p = put_hex(p, h, sizeof(h));
    *p = 0;
    crypto_wipe(h, sizeof(h));
    return 0;
}

int pwhash_valid(const char *stored){
    uint32_t it;
    uint8_t s[PWHASH_SALT_LEN], h[PWHASH_HASH_LEN];
    return parse(stored, &it, s, h) == 0;
}

int pwhash_verify(const void *secret, size_t len, const char *stored){
    uint32_t it;
    uint8_t salt[PWHASH_SALT_LEN], want[PWHASH_HASH_LEN], got[PWHASH_HASH_LEN];
    if(parse(stored, &it, salt, want) < 0){
        return -1;
    }
    if(pbkdf2_hmac_sha256(secret, len, salt, sizeof(salt), it, got, sizeof(got)) < 0){
        return -1;
    }
    int ok = crypto_ct_equal(got, want, sizeof(got));
    crypto_wipe(got, sizeof(got));
    return ok;
}
