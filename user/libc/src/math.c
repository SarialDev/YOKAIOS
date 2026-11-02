/* libm: aritmetica in SSE2, functiile transcendente in x87 (80 de biti intern, rezultat rotunjit la double).
 * Kernelul salveaza starea FPU la fiecare comutare (arch/x86_64/fpu), deci x87 si SSE sunt sigure in userland.
 * Controlul x87 ramane cel implicit dupa fninit: precizie 64 de biti, rotunjire la cel mai apropiat. */
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline uint64_t bits_of(double x){ uint64_t u; memcpy(&u, &x, 8); return u; }
static inline double from_bits(uint64_t u){ double x; memcpy(&x, &u, 8); return x; }

#define SIGN_BIT 0x8000000000000000ULL
#define EXP_MASK 0x7FF0000000000000ULL
#define MANT_MASK 0x000FFFFFFFFFFFFFULL

double fabs(double x){ return from_bits(bits_of(x) & ~SIGN_BIT); }
double copysign(double x, double y){ return from_bits((bits_of(x) & ~SIGN_BIT) | (bits_of(y) & SIGN_BIT)); }

double sqrt(double x){
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static int is_nan(double x){ return (bits_of(x) & ~SIGN_BIT) > EXP_MASK; }
static int is_inf(double x){ return (bits_of(x) & ~SIGN_BIT) == EXP_MASK; }

double trunc(double x){
    uint64_t u = bits_of(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if(e >= 52){
        return x;                               // deja intreg (sau inf/NaN)
    }
    if(e < 0){
        return from_bits(u & SIGN_BIT);         // +-0
    }
    return from_bits(u & ~(MANT_MASK >> e));
}

double floor(double x){
    double t = trunc(x);
    return (x < 0 && t != x) ? t - 1.0 : t;
}

double ceil(double x){
    double t = trunc(x);
    return (x > 0 && t != x) ? t + 1.0 : t;
}

double round(double x){
    double t = trunc(x);
    double d = fabs(x - t);
    if(d >= 0.5){
        t += x < 0 ? -1.0 : 1.0;
    }
    return t;
}

double rint(double x){
    double t = floor(x);
    double d = x - t;
    if(d > 0.5 || (d == 0.5 && fmod(t, 2.0) != 0.0)){
        t += 1.0;
    }
    return t;
}

double fmax(double x, double y){
    if(is_nan(x)) return y;
    if(is_nan(y)) return x;
    return x > y ? x : y;
}

double fmin(double x, double y){
    if(is_nan(x)) return y;
    if(is_nan(y)) return x;
    return x < y ? x : y;
}

/* x * 2^e exact, cu o singura rotunjire (cand rezultatul e subnormal) */
double scalbn(double x, int e){
    if(x == 0 || is_nan(x) || is_inf(x)){
        return x;
    }
    if(e > 3000) e = 3000;
    if(e < -3000) e = -3000;
    while(e > 1000){
        x *= 0x1p1000;
        e -= 1000;
    }
    while(e < -1000){
        x *= 0x1p-1000;
        e += 1000;
    }
    uint64_t u = (uint64_t)(e + 1023) << 52;
    return x * from_bits(u);
}

double ldexp(double x, int e){ return scalbn(x, e); }

double frexp(double x, int *e){
    *e = 0;
    if(x == 0 || is_nan(x) || is_inf(x)){
        return x;
    }
    uint64_t u = bits_of(x);
    int ex = (int)((u >> 52) & 0x7FF);
    if(ex == 0){                                // subnormal: normalizeaza
        x *= 0x1p64;
        u = bits_of(x);
        ex = (int)((u >> 52) & 0x7FF);
        *e = -64;
    }
    *e += ex - 1022;
    u = (u & ~EXP_MASK) | (uint64_t)1022 << 52;
    return from_bits(u);
}

double modf(double x, double *ip){
    if(is_inf(x)){
        *ip = x;
        return copysign(0.0, x);
    }
    double t = trunc(x);
    *ip = t;
    if(is_nan(x)){
        return x;
    }
    return copysign(x - t, x);
}

/* fprem e exact */
double fmod(double x, double y){
    if(is_nan(x) || is_nan(y) || is_inf(x) || y == 0){
        return from_bits(0x7FF8000000000000ULL);
    }
    if(is_inf(y)){
        return x;
    }
    double r;
    unsigned short sw;
    __asm__ volatile(
        "fldl %3\n\t"
        "fldl %2\n\t"
        "1: fprem\n\t"
        "fnstsw %1\n\t"
        "testb $4, %h1\n\t"
        "jnz 1b\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %0"
        : "=m"(r), "=a"(sw) : "m"(x), "m"(y) : "cc");
    return r;
}

/* 2^t pe x87: t in st(0) la intrare; rezultat in st(0) */
#define X87_EXP2_TAIL \
    "fld %%st(0)\n\t" \
    "frndint\n\t" \
    "fxch\n\t" \
    "fsub %%st(1), %%st\n\t" \
    "f2xm1\n\t" \
    "fld1\n\t" \
    "faddp\n\t" \
    "fscale\n\t" \
    "fstp %%st(1)\n\t"

double exp(double x){
    if(is_nan(x)) return x;
    if(x > 709.79) return HUGE_VAL;
    if(x < -745.2) return 0.0;
    double r;
    __asm__ volatile(
        "fldl2e\n\t"
        "fmull %1\n\t"
        X87_EXP2_TAIL
        "fstpl %0"
        : "=m"(r) : "m"(x));
    return r;
}

double exp2(double x){
    if(is_nan(x)) return x;
    if(x > 1024) return HUGE_VAL;
    if(x < -1080) return 0.0;
    double r;
    __asm__ volatile(
        "fldl %1\n\t"
        X87_EXP2_TAIL
        "fstpl %0"
        : "=m"(r) : "m"(x));
    return r;
}

static double ylog2x(double k, double x){       // k * log2(x), fyl2x
    double r;
    __asm__ volatile(
        "fldl %1\n\t"
        "fldl %2\n\t"
        "fyl2x\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(k), "m"(x));
    return r;
}

double log2(double x){
    if(is_nan(x) || x == 0 || x < 0 || is_inf(x)){
        if(x == 0) return -HUGE_VAL;
        if(x < 0 || is_nan(x)) return from_bits(0x7FF8000000000000ULL);
        return x;
    }
    return ylog2x(1.0, x);
}

double log(double x){
    if(x == 0) return -HUGE_VAL;
    if(x < 0 || is_nan(x)) return from_bits(0x7FF8000000000000ULL);
    if(is_inf(x)) return x;
    double r;
    __asm__ volatile(
        "fldln2\n\t"
        "fldl %1\n\t"
        "fyl2x\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x));
    return r;
}

double log10(double x){
    if(x == 0) return -HUGE_VAL;
    if(x < 0 || is_nan(x)) return from_bits(0x7FF8000000000000ULL);
    if(is_inf(x)) return x;
    double r;
    __asm__ volatile(
        "fldlg2\n\t"
        "fldl %1\n\t"
        "fyl2x\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x));
    return r;
}

static int is_int(double y){ return y == trunc(y); }
static int is_odd_int(double y){
    return fabs(y) < 0x1p53 && is_int(y) && ((long)y & 1);
}

double pow(double x, double y){
    if(y == 0) return 1.0;
    if(x == 1.0) return 1.0;
    if(is_nan(x) || is_nan(y)) return from_bits(0x7FF8000000000000ULL);
    if(x == -1.0 && is_inf(y)) return 1.0;
    if(is_inf(y)){
        double ax = fabs(x);
        if((ax < 1.0) == (y < 0)) return HUGE_VAL;
        return 0.0;
    }
    if(x == 0){
        int odd = is_odd_int(y);
        if(y < 0) return odd ? copysign(HUGE_VAL, x) : HUGE_VAL;
        return odd ? x : 0.0;
    }
    if(is_inf(x)){
        int odd = is_odd_int(y);
        if(x > 0) return y < 0 ? 0.0 : HUGE_VAL;
        if(y < 0) return odd ? -0.0 : 0.0;
        return odd ? -HUGE_VAL : HUGE_VAL;
    }
    int neg = 0;
    if(x < 0){
        if(!is_int(y)) return from_bits(0x7FF8000000000000ULL);
        neg = is_odd_int(y);
        x = -x;
    }
    // exponent intreg mic: inmultiri repetate (exact cat timp rezultatul e reprezentabil)
    if(is_int(y) && fabs(y) <= 64){
        long n = (long)fabs(y);
        double r = 1.0, b = x;
        while(n){
            if(n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        if(y < 0) r = 1.0 / r;
        return neg ? -r : r;
    }
    double t = y * log2(x);                     // doar pentru decizia de depasire; calculul real ramane pe 80 de biti
    if(t > 1100) return neg ? -HUGE_VAL : HUGE_VAL;
    if(t < -1100) return neg ? -0.0 : 0.0;
    double r;
    __asm__ volatile(
        "fldl %1\n\t"
        "fldl %2\n\t"
        "fyl2x\n\t"
        X87_EXP2_TAIL
        "fstpl %0"
        : "=m"(r) : "m"(y), "m"(x));
    return neg ? -r : r;
}

double hypot(double x, double y){
    if(is_inf(x) || is_inf(y)) return HUGE_VAL;
    if(is_nan(x) || is_nan(y)) return from_bits(0x7FF8000000000000ULL);
    x = fabs(x);
    y = fabs(y);
    if(x < y){ double t = x; x = y; y = t; }
    if(x == 0) return 0.0;
    double q = y / x;
    return x * sqrt(1.0 + q * q);
}

/* sin/cos/tan: x87 fsin/fcos/fptan accepta |x| < 2^63; peste: reducere cu fprem1 modulo 2*pi (pi pe 64 de biti) */
static double reduce_big(double x){
    double r;
    unsigned short sw;
    __asm__ volatile(
        "fldpi\n\t"
        "fadd %%st(0), %%st\n\t"
        "fldl %2\n\t"
        "1: fprem1\n\t"
        "fnstsw %1\n\t"
        "testb $4, %h1\n\t"
        "jnz 1b\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %0"
        : "=m"(r), "=a"(sw) : "m"(x) : "cc");
    return r;
}

double sin(double x){
    if(is_nan(x) || is_inf(x)) return from_bits(0x7FF8000000000000ULL);
    if(fabs(x) >= 0x1p62) x = reduce_big(x);
    double r;
    __asm__ volatile("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double cos(double x){
    if(is_nan(x) || is_inf(x)) return from_bits(0x7FF8000000000000ULL);
    if(fabs(x) >= 0x1p62) x = reduce_big(x);
    double r;
    __asm__ volatile("fldl %1\n\tfcos\n\tfstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double tan(double x){
    if(is_nan(x) || is_inf(x)) return from_bits(0x7FF8000000000000ULL);
    if(fabs(x) >= 0x1p62) x = reduce_big(x);
    double r;
    __asm__ volatile("fldl %1\n\tfptan\n\tfstp %%st(0)\n\tfstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double atan2(double y, double x){
    if(is_nan(x) || is_nan(y)) return from_bits(0x7FF8000000000000ULL);
    double r;
    __asm__ volatile("fldl %1\n\tfldl %2\n\tfpatan\n\tfstpl %0" : "=m"(r) : "m"(y), "m"(x));
    return r;
}

double atan(double x){ return atan2(x, 1.0); }

double asin(double x){
    if(is_nan(x) || fabs(x) > 1.0) return from_bits(0x7FF8000000000000ULL);
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x){
    if(is_nan(x) || fabs(x) > 1.0) return from_bits(0x7FF8000000000000ULL);
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

double sinh(double x){
    double a = fabs(x);
    double r;
    if(a < 0.5){
        double x2 = a * a;                      // serie Taylor: fara pierdere de precizie langa 0
        r = a * (1.0 + x2 / 6.0 * (1.0 + x2 / 20.0 * (1.0 + x2 / 42.0 * (1.0 + x2 / 72.0 * (1.0 + x2 / 110.0)))));
    } else {
        double e = exp(a);
        r = (e - 1.0 / e) * 0.5;
    }
    return copysign(r, x);
}

double cosh(double x){
    double e = exp(fabs(x));
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x){
    double a = fabs(x);
    if(a > 22.0) return copysign(1.0, x);
    double r;
    if(a < 0.5){
        r = sinh(a) / cosh(a);
    } else {
        double e = exp(2.0 * a);
        r = (e - 1.0) / (e + 1.0);
    }
    return copysign(r, x);
}
