#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

int abs(int x){ return x < 0 ? -x : x; }
long labs(long x){ return x < 0 ? -x : x; }
long long llabs(long long x){ return x < 0 ? -x : x; }
div_t div(int a, int b){ div_t d = {a / b, a % b}; return d; }
ldiv_t ldiv(long a, long b){ ldiv_t d = {a / b, a % b}; return d; }

static unsigned long long parse(const char *s, char **end, int base, int *neg, int *overflow){
    const char *p = s;
    unsigned long long v = 0;
    int any = 0;
    *neg = 0;
    *overflow = 0;
    while(isspace((unsigned char)*p)){
        p++;
    }
    if(*p == '+' || *p == '-'){
        *neg = *p == '-';
        p++;
    }
    if((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])){
        p += 2;
        base = 16;
    } else if(base == 0){
        base = p[0] == '0' ? 8 : 10;
    }
    if(base < 2 || base > 36){
        if(end){
            *end = (char *)s;
        }
        errno = EINVAL;
        return 0;
    }
    for(;; p++){
        int d;
        if(isdigit((unsigned char)*p)){
            d = *p - '0';
        } else if(isalpha((unsigned char)*p)){
            d = tolower((unsigned char)*p) - 'a' + 10;
        } else {
            break;
        }
        if(d >= base){
            break;
        }
        any = 1;
        if(v > (ULLONG_MAX - (unsigned)d) / (unsigned)base){
            *overflow = 1;
            v = ULLONG_MAX;
        } else if(!*overflow){
            v = v * (unsigned)base + (unsigned)d;
        }
    }
    if(end){
        *end = (char *)(any ? p : s);
    }
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base){
    int neg, ov;
    unsigned long long v = parse(s, end, base, &neg, &ov);
    if(ov){
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base){
    int neg, ov;
    unsigned long long v = parse(s, end, base, &neg, &ov);
    if(ov || (!neg && v > (unsigned long long)LLONG_MAX) || (neg && v > (unsigned long long)LLONG_MAX + 1)){
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? (long long)(0 - v) : (long long)v;
}

long strtol(const char *s, char **end, int base){ return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base){ return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s){ return (int)strtol(s, NULL, 10); }
long atol(const char *s){ return strtol(s, NULL, 10); }
long long atoll(const char *s){ return strtoll(s, NULL, 10); }

static unsigned long long rand_state = 1;

void srand(unsigned seed){
    rand_state = seed;
}

int rand(void){
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rand_state >> 33) & RAND_MAX);
}

static void swap_bytes(char *a, char *b, size_t n){
    while(n--){
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

typedef int (*cmp_fn)(const void *, const void *);

static void insertion(char *base, size_t n, size_t sz, cmp_fn cmp){
    for(size_t i = 1; i < n; i++){
        for(size_t j = i; j > 0 && cmp(base + (j - 1) * sz, base + j * sz) > 0; j--){
            swap_bytes(base + (j - 1) * sz, base + j * sz, sz);
        }
    }
}

// Quicksort cu mediana din trei; recursie doar pe partea mica (stiva O(log n))
static void quick(char *base, size_t n, size_t sz, cmp_fn cmp){
    while(n > 12){
        char *a = base, *m = base + (n / 2) * sz, *z = base + (n - 1) * sz;
        if(cmp(a, m) > 0) swap_bytes(a, m, sz);
        if(cmp(m, z) > 0) swap_bytes(m, z, sz);
        if(cmp(a, m) > 0) swap_bytes(a, m, sz);
        swap_bytes(m, z - sz, sz);                  // pivotul in penultima pozitie
        char *pivot = z - sz;
        size_t i = 0, j = n - 2;
        for(;;){
            while(cmp(base + (++i) * sz, pivot) < 0){
            }
            while(j > 0 && cmp(base + (--j) * sz, pivot) > 0){
            }
            if(i >= j){
                break;
            }
            swap_bytes(base + i * sz, base + j * sz, sz);
        }
        swap_bytes(base + i * sz, pivot, sz);
        size_t left = i, right = n - i - 1;
        if(left < right){
            quick(base, left, sz, cmp);
            base += (i + 1) * sz;
            n = right;
        } else {
            quick(base + (i + 1) * sz, right, sz, cmp);
            n = left;
        }
    }
    insertion(base, n, sz, cmp);
}

void qsort(void *base, size_t n, size_t size, cmp_fn cmp){
    if(n > 1 && size){
        quick(base, n, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, cmp_fn cmp){
    size_t lo = 0, hi = n;
    while(lo < hi){
        size_t mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(key, p);
        if(c == 0){
            return (void *)p;
        }
        if(c < 0){
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}
