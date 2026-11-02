/* Conversii double <-> text, exacte (aritmetica pe numere mari): strtod cu rotunjire corecta (zecimal si hex),
 * si cifrele zecimale ale unui double pentru printf (%e %f %g %a). Fara SSE in logica de baza: doar intregi,
 * double apare doar in interfata si la reasamblarea rezultatului. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include "internal.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned __int128 u128;

#define NW 132
typedef struct {
    u32 w[NW];
    int n;
} Big;

static void b_trim(Big *b){
    while(b->n > 0 && b->w[b->n - 1] == 0){
        b->n--;
    }
}

static void b_set_u64(Big *b, u64 v){
    b->n = 0;
    while(v){
        b->w[b->n++] = (u32)v;
        v >>= 32;
    }
}

static void b_mul_small(Big *b, u32 m){
    u64 c = 0;
    for(int i = 0; i < b->n; i++){
        u64 t = (u64)b->w[i] * m + c;
        b->w[i] = (u32)t;
        c = t >> 32;
    }
    if(c && b->n < NW){
        b->w[b->n++] = (u32)c;
    }
}

static void b_add_small(Big *b, u32 a){
    u64 c = a;
    for(int i = 0; c && i < b->n; i++){
        u64 t = (u64)b->w[i] + c;
        b->w[i] = (u32)t;
        c = t >> 32;
    }
    if(c && b->n < NW){
        b->w[b->n++] = (u32)c;
    }
}

static u32 b_div_small(Big *b, u32 d){
    u64 r = 0;
    for(int i = b->n - 1; i >= 0; i--){
        u64 t = (r << 32) | b->w[i];
        b->w[i] = (u32)(t / d);
        r = t % d;
    }
    b_trim(b);
    return (u32)r;
}

static void b_shl(Big *b, int s){
    if(b->n == 0 || s == 0){
        return;
    }
    int ws = s / 32, bs = s % 32;
    int n = b->n;
    if(n + ws + 1 > NW){
        return;                                 // nu se intampla pentru intrarile limitate de apelanti
    }
    for(int i = n + ws; i >= 0; i--){
        u64 lo = i - ws >= 0 && i - ws < n ? b->w[i - ws] : 0;
        u64 lo2 = i - ws - 1 >= 0 && i - ws - 1 < n ? b->w[i - ws - 1] : 0;
        b->w[i] = bs ? (u32)((lo << bs) | (lo2 >> (32 - bs))) : (u32)lo;
    }
    b->n = n + ws + 1;
    b_trim(b);
}

static void b_shr1(Big *b){
    u32 carry = 0;
    for(int i = b->n - 1; i >= 0; i--){
        u32 nc = b->w[i] & 1;
        b->w[i] = (b->w[i] >> 1) | (carry << 31);
        carry = nc;
    }
    b_trim(b);
}

static int b_bitlen(const Big *b){
    if(b->n == 0){
        return 0;
    }
    return (b->n - 1) * 32 + (32 - __builtin_clz(b->w[b->n - 1]));
}

static int b_cmp(const Big *a, const Big *b){
    if(a->n != b->n){
        return a->n < b->n ? -1 : 1;
    }
    for(int i = a->n - 1; i >= 0; i--){
        if(a->w[i] != b->w[i]){
            return a->w[i] < b->w[i] ? -1 : 1;
        }
    }
    return 0;
}

static void b_sub(Big *a, const Big *b){        // a -= b, a >= b
    u64 borrow = 0;
    for(int i = 0; i < a->n; i++){
        u64 bv = i < b->n ? b->w[i] : 0;
        u64 t = (u64)a->w[i] - bv - borrow;
        a->w[i] = (u32)t;
        borrow = (t >> 32) & 1;
    }
    b_trim(a);
}

static void b_pow10(Big *b, int e){             // b *= 10^e
    while(e >= 9){
        b_mul_small(b, 1000000000u);
        e -= 9;
    }
    static const u32 p10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if(e > 0){
        b_mul_small(b, p10[e]);
    }
}

static int bitlen128(u128 q){
    u64 hi = (u64)(q >> 64);
    if(hi){
        return 128 - __builtin_clzll(hi);
    }
    u64 lo = (u64)q;
    return lo ? 64 - __builtin_clzll(lo) : 0;
}

/* q * 2^exp2 (+ sticky: valoarea reala e putin mai mare), rotunjit la cel mai apropiat double (egalitate -> par) */
static double make_double(u128 q, int sticky, int exp2){
    if(q == 0){
        return 0.0;
    }
    int L = bitlen128(q);
    int e = L - 1 + exp2;
    if(e > 1023){
        errno = ERANGE;
        return HUGE_VAL;
    }
    int keep = e < -1022 ? e + 1075 : 53;
    if(keep < 0){
        errno = ERANGE;
        return 0.0;
    }
    int shift = L - keep;
    u128 mant;
    if(shift <= 0){
        mant = q << -shift;
    } else {
        mant = shift >= 128 ? 0 : q >> shift;
        u128 rem = shift >= 128 ? q : q & (((u128)1 << shift) - 1);
        u128 half = (u128)1 << (shift - 1);
        if(rem > half || (rem == half && (sticky || (mant & 1)))){
            mant++;
        }
    }
    double r = (double)(u64)mant;
    r = scalbn(r, exp2 + shift);
    if(r == 0.0){
        errno = ERANGE;
    }
    return r;
}

/* ---------- strtod ---------- */

#define MAXD 800

static int hexval(int c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int match_ci(const char *p, const char *word){
    for(; *word; p++, word++){
        if(tolower((unsigned char)*p) != *word){
            return 0;
        }
    }
    return 1;
}

static double parse_hex(const char *p, int neg, const char *start, const char **endp){
    // p dupa "0x"
    u64 mant = 0;
    int sticky = 0, exp2 = 0, any = 0, dot = 0;
    for(;; p++){
        if(*p == '.' && !dot){
            dot = 1;
            continue;
        }
        int h = hexval((unsigned char)*p);
        if(h < 0){
            break;
        }
        any = 1;
        if(mant >> 60 == 0){
            mant = (mant << 4) | (u64)h;
            if(dot) exp2 -= 4;
        } else {
            if(h) sticky = 1;
            if(!dot) exp2 += 4;
        }
    }
    if(!any){
        *endp = start;
        return 0.0;
    }
    if((*p == 'p' || *p == 'P')){
        const char *q = p + 1;
        int eneg = 0;
        if(*q == '+' || *q == '-'){
            eneg = *q == '-';
            q++;
        }
        if(isdigit((unsigned char)*q)){
            long ev = 0;
            for(; isdigit((unsigned char)*q); q++){
                if(ev < 100000) ev = ev * 10 + (*q - '0');
            }
            exp2 += (int)(eneg ? -ev : ev);
            p = q;
        }
    }
    *endp = p;
    double r = make_double(mant, sticky, exp2);
    return neg ? -r : r;
}

double strtod(const char *s, char **end){
    const char *p = s;
    while(isspace((unsigned char)*p)){
        p++;
    }
    int neg = 0;
    if(*p == '+' || *p == '-'){
        neg = *p == '-';
        p++;
    }
    if(match_ci(p, "inf")){
        p += match_ci(p, "infinity") ? 8 : 3;
        if(end) *end = (char *)p;
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if(match_ci(p, "nan")){
        p += 3;
        if(*p == '('){
            const char *q = p + 1;
            while(isalnum((unsigned char)*q) || *q == '_') q++;
            if(*q == ')') p = q + 1;
        }
        if(end) *end = (char *)p;
        return neg ? -__builtin_nan("") : __builtin_nan("");
    }
    if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (hexval((unsigned char)p[2]) >= 0 ||
       (p[2] == '.' && hexval((unsigned char)p[3]) >= 0))){
        const char *e2;
        double r = parse_hex(p + 2, neg, s, &e2);
        if(end) *end = (char *)(e2 == s ? s : e2);
        return r;
    }
    char dig[MAXD + 2];
    int nd = 0, exp10 = 0, sticky = 0, seen = 0, any = 0, dot = 0;
    for(;; p++){
        if(*p == '.' && !dot){
            dot = 1;
            continue;
        }
        if(!isdigit((unsigned char)*p)){
            break;
        }
        any = 1;
        int d = *p - '0';
        if(!seen && d == 0){
            if(dot) exp10--;                    // zerouri de la inceputul partii fractionare
            continue;
        }
        seen = 1;
        if(nd < MAXD){
            dig[nd++] = (char)d;
            if(dot) exp10--;
        } else {
            if(d) sticky = 1;
            if(!dot) exp10++;
        }
    }
    if(!any){
        if(end) *end = (char *)s;
        return 0.0;
    }
    if(*p == 'e' || *p == 'E'){
        const char *q = p + 1;
        int eneg = 0;
        if(*q == '+' || *q == '-'){
            eneg = *q == '-';
            q++;
        }
        if(isdigit((unsigned char)*q)){
            long ev = 0;
            for(; isdigit((unsigned char)*q); q++){
                if(ev < 100000) ev = ev * 10 + (*q - '0');
            }
            exp10 += (int)(eneg ? -ev : ev);
            p = q;
        }
    }
    if(end) *end = (char *)p;
    while(nd > 0 && dig[nd - 1] == 0){
        nd--;
        exp10++;
    }
    if(nd == 0){
        return neg ? -0.0 : 0.0;
    }
    if(nd + exp10 > 310){
        errno = ERANGE;
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if(nd + exp10 < -330){
        errno = ERANGE;
        return neg ? -0.0 : 0.0;
    }
    if(sticky){
        dig[nd++] = 1;
        exp10--;
    }
    Big D;
    D.n = 0;
    for(int i = 0; i < nd;){
        int take = nd - i < 9 ? nd - i : 9;
        u32 chunk = 0, mul = 1;
        for(int j = 0; j < take; j++){
            chunk = chunk * 10 + (u32)dig[i + j];
            mul *= 10;
        }
        b_mul_small(&D, mul);
        b_add_small(&D, chunk);
        i += take;
    }
    double r;
    if(exp10 >= 0){
        b_pow10(&D, exp10);
        int bl = b_bitlen(&D);
        if(bl > 1100){
            errno = ERANGE;
            r = HUGE_VAL;
        } else {
            int shift = bl > 64 ? bl - 64 : 0;
            u64 top = 0;
            int st = 0;
            for(int i = 0; i < bl; i++){
                // bitii de la shift in sus formeaza q; cei de sub shift dau sticky
                int w = i / 32, b = i % 32;
                int bit = (D.w[w] >> b) & 1;
                if(i < shift){
                    st |= bit;
                } else {
                    top |= (u64)bit << (i - shift);
                }
            }
            r = make_double(top, st, shift);
        }
    } else {
        int n = -exp10;
        Big T;
        T.n = 0;
        b_set_u64(&T, 1);
        b_pow10(&T, n);
        int s = 67 + b_bitlen(&T) - b_bitlen(&D);
        if(s < 0) s = 0;
        b_shl(&D, s);                           // R = D << s
        // Q = R / T, Q are 67..68 de biti
        int qb = b_bitlen(&D) - b_bitlen(&T) + 1;
        if(qb < 1) qb = 1;
        Big Ti = T;
        b_shl(&Ti, qb - 1);
        u128 Q = 0;
        for(int i = qb - 1; i >= 0; i--){
            if(b_cmp(&D, &Ti) >= 0){
                b_sub(&D, &Ti);
                Q |= (u128)1 << i;
            }
            b_shr1(&Ti);
        }
        r = make_double(Q, D.n > 0, -s);
    }
    return neg ? -r : r;
}

float strtof(const char *s, char **end){ return (float)strtod(s, end); }
double atof(const char *s){ return strtod(s, NULL); }

/* ---------- cifre zecimale pentru printf ---------- */

/* v finit si > 0. mode 2: ndig cifre semnificative; mode 3: ndig cifre dupa virgula.
 * out primeste nd cifre ('0'..'9'), valoarea = 0.CIFRE * 10^decpt. Returneaza nd (0 daca se rotunjeste la zero), -1 fara memorie.
 * out trebuie sa aiba loc pentru ndig + 2 cifre. */
int __fp_digits(double v, int mode, int ndig, char *out, int *decpt){
    u64 bits;
    memcpy(&bits, &v, 8);
    int ex = (int)((bits >> 52) & 0x7FF);
    u64 m = bits & 0x000FFFFFFFFFFFFFULL;
    if(ex == 0){
        ex = 1;
    } else {
        m |= 1ULL << 52;
    }
    int e2 = ex - 1075;
    size_t cap = 1500 + (size_t)ndig;
    char *A = malloc(cap);
    Big *F = malloc(sizeof(Big));
    if(!A || !F){
        free(A);
        free(F);
        return -1;
    }
    int ilen = 0, alen = 0;
    F->n = 0;
    int k = 0;
    if(e2 >= 0){
        Big N;
        b_set_u64(&N, m);
        b_shl(&N, e2);
        char tmp[360];
        int tl = 0;
        while(N.n > 0){
            u32 r = b_div_small(&N, 1000000000u);
            for(int j = 0; j < 9; j++){
                tmp[tl++] = (char)('0' + r % 10);
                r /= 10;
            }
        }
        while(tl > 0 && tmp[tl - 1] == '0'){
            tl--;
        }
        for(int i = tl - 1; i >= 0; i--){
            A[alen++] = tmp[i];
        }
        ilen = alen;
    } else {
        k = -e2;
        u64 ip = k < 64 ? m >> k : 0;
        u64 f = k < 64 ? m & ((1ULL << k) - 1) : m;
        char tmp[24];
        int tl = 0;
        while(ip){
            tmp[tl++] = (char)('0' + ip % 10);
            ip /= 10;
        }
        for(int i = tl - 1; i >= 0; i--){
            A[alen++] = tmp[i];
        }
        ilen = alen;
        b_set_u64(F, f);
    }
    // fraction: cifre pana avem destule
    for(;;){
        int fz = 0;
        while(fz < alen && A[fz] == '0'){
            fz++;
        }
        int have = mode == 2 ? (fz < alen ? alen - fz : 0) : alen;
        int need = mode == 2 ? ndig + 1 : ilen + ndig + 1;
        if(have >= need || F->n == 0 || (size_t)alen + 9 >= cap){
            break;
        }
        b_mul_small(F, 1000000000u);
        // q = F >> k ; F &= 2^k - 1
        int wi = k / 32, bs = k % 32;
        u64 lo = wi < F->n ? F->w[wi] : 0;
        u64 hi = wi + 1 < F->n ? F->w[wi + 1] : 0;
        u32 q = bs ? (u32)(((lo >> bs) | (hi << (32 - bs))) & 0xFFFFFFFFu) : (u32)lo;
        if(bs){
            if(wi < F->n){
                F->w[wi] &= (1u << bs) - 1;
                F->n = wi + 1;
            }
        } else if(wi < F->n){
            F->n = wi;
        }
        b_trim(F);
        for(int j = 8; j >= 0; j--){
            A[alen + j] = (char)('0' + q % 10);
            q /= 10;
        }
        alen += 9;
    }
    int fz = 0;
    while(fz < alen && A[fz] == '0'){
        fz++;
    }
    int c = mode == 2 ? fz + ndig : ilen + ndig;
    int nd;
    if(mode == 3 && c < fz){
        nd = 0;
        *decpt = 0;
        goto done;
    }
    {
        int r = c < alen ? A[c] - '0' : 0;
        int st = F->n > 0;
        for(int i = c + 1; !st && i < alen; i++){
            if(A[i] != '0') st = 1;
        }
        int odd = c > 0 && c - 1 < alen && ((A[c - 1] - '0') & 1);
        int up = r > 5 || (r == 5 && (st || odd));
        int n = c - fz;
        if(n <= 0){
            if(up){
                out[0] = '1';
                nd = 1;
                *decpt = ilen - c + 1;
            } else {
                nd = 0;
                *decpt = 0;
            }
            goto done;
        }
        for(int i = 0; i < n; i++){
            out[i] = fz + i < alen ? A[fz + i] : '0';
        }
        *decpt = ilen - fz;
        nd = n;
        if(up){
            int i = n - 1;
            while(i >= 0 && out[i] == '9'){
                out[i] = '0';
                i--;
            }
            if(i >= 0){
                out[i]++;
            } else {
                // 99..9 -> 100..0
                memmove(out + 1, out, (size_t)n);
                out[0] = '1';
                (*decpt)++;
                nd = mode == 3 ? n + 1 : n;
            }
        }
    }
done:
    free(A);
    free(F);
    return nd;
}

static char *put_exp(char *p, int x, char echar, int mind){
    *p++ = echar;
    if(x < 0){
        *p++ = '-';
        x = -x;
    } else {
        *p++ = '+';
    }
    char t[8];
    int tl = 0;
    while(x){
        t[tl++] = (char)('0' + x % 10);
        x /= 10;
    }
    while(tl < mind){
        t[tl++] = '0';
    }
    while(tl){
        *p++ = t[--tl];
    }
    return p;
}

static char dgt(const char *d, int nd, int i){
    return i >= 0 && i < nd ? d[i] : '0';
}

/* |v| finit; conv in e E f F g G; prec >= 0. Rezultat alocat cu malloc (fara semn), *len = lungimea. NULL fara memorie */
char *__fp_format(double v, char conv, int prec, int alt, int *len){
    int upper = conv == 'E' || conv == 'F' || conv == 'G';
    char lc = (char)(conv | 0x20);
    if(prec > 4000){
        prec = 4000;
    }
    char *buf = malloc((size_t)prec + 400);
    char *dg = malloc((size_t)prec + 400);
    if(!buf || !dg){
        free(buf);
        free(dg);
        return NULL;
    }
    char *p = buf;
    int nd, decpt;
    int zero = v == 0.0;
    int style = lc;                             // 'e' sau 'f'
    int P = prec;
    if(lc == 'g'){
        P = prec == 0 ? 1 : prec;
    }
    if(zero){
        nd = 0;
        decpt = 1;
    } else if(lc == 'f'){
        nd = __fp_digits(v, 3, prec, dg, &decpt);
    } else {
        nd = __fp_digits(v, 2, lc == 'e' ? prec + 1 : P, dg, &decpt);
    }
    if(nd < 0){
        free(buf);
        free(dg);
        return NULL;
    }
    int fprec = prec;
    if(lc == 'g'){
        int X = zero ? 0 : decpt - 1;
        if(X < -4 || X >= P){
            style = 'e';
            fprec = P - 1;
        } else {
            style = 'f';
            fprec = P - 1 - X;
        }
    }
    if(style == 'e'){
        int x = zero ? 0 : decpt - 1;
        *p++ = zero ? '0' : dgt(dg, nd, 0);
        if(fprec > 0 || alt){
            *p++ = '.';
        }
        for(int i = 1; i <= fprec; i++){
            *p++ = zero ? '0' : dgt(dg, nd, i);
        }
        p = put_exp(p, x, upper ? 'E' : 'e', 2);
    } else {
        if(zero || nd == 0 || decpt <= 0){
            *p++ = '0';
        } else {
            for(int i = 0; i < decpt; i++){
                *p++ = dgt(dg, nd, i);
            }
        }
        if(fprec > 0 || alt){
            *p++ = '.';
        }
        for(int j = 0; j < fprec; j++){
            *p++ = zero ? '0' : dgt(dg, nd, decpt + j);
        }
    }
    if(lc == 'g' && !alt){
        // taie zerourile finale din partea fractionara (inainte de exponent, daca exista)
        char *epos = NULL;
        for(char *q = buf; q < p; q++){
            if(*q == 'e' || *q == 'E'){
                epos = q;
                break;
            }
        }
        char *mant_end = epos ? epos : p;
        char *dot = NULL;
        for(char *q = buf; q < mant_end; q++){
            if(*q == '.'){
                dot = q;
                break;
            }
        }
        if(dot){
            char *z = mant_end;
            while(z > dot + 1 && z[-1] == '0'){
                z--;
            }
            if(z == dot + 1){
                z = dot;
            }
            if(epos){
                memmove(z, epos, (size_t)(p - epos));
                p = z + (p - epos);
            } else {
                p = z;
            }
        }
    }
    free(dg);
    *len = (int)(p - buf);
    return buf;
}

/* %a: v finit >= 0; prec < 0 = cate cifre trebuie */
char *__fp_format_hex(double v, int prec, int upper, int alt, int *len){
    u64 bits;
    memcpy(&bits, &v, 8);
    int ex = (int)((bits >> 52) & 0x7FF);
    u64 m = bits & 0x000FFFFFFFFFFFFFULL;
    int e;
    u64 lead;
    if(v == 0.0){
        lead = 0;
        e = 0;
    } else if(ex == 0){
        lead = 0;
        e = -1022;
    } else {
        lead = 1;
        e = ex - 1023;
    }
    if(prec >= 0 && prec < 13){
        int sh = 4 * (13 - prec);
        u64 rem = m & ((1ULL << sh) - 1);
        u64 half = 1ULL << (sh - 1);
        m >>= sh;
        if(rem > half || (rem == half && ((m & 1) || (prec == 0 && (lead & 1))))){
            m++;
            if(m >> (4 * prec) && prec < 13){
                if(m >> (4 * prec)){
                    m &= (1ULL << (4 * prec)) - 1;
                    lead++;
                }
            }
        }
        m <<= sh;
    }
    char *buf = malloc((size_t)(prec > 0 ? prec : 0) + 40);
    if(!buf){
        return NULL;
    }
    const char *hd = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = buf;
    *p++ = '0';
    *p++ = upper ? 'X' : 'x';
    *p++ = (char)('0' + lead);
    int nfrac = 13;
    if(prec < 0){
        while(nfrac > 0 && ((m >> (52 - 4 * nfrac)) & 0xF) == 0){
            nfrac--;
        }
    } else {
        nfrac = prec;
    }
    if(nfrac > 0 || alt){
        *p++ = '.';
    }
    for(int i = 0; i < nfrac; i++){
        *p++ = i < 13 ? hd[(m >> (48 - 4 * i)) & 0xF] : '0';
    }
    p = put_exp(p, e, upper ? 'P' : 'p', 1);
    *len = (int)(p - buf);
    return buf;
}
