#include "lib/string.h"

// rep stosb / rep movsb: compilatorul nu le poate transforma inapoi in apeluri la memset/memcpy
extern "C" void* memset(void* dst, int value, size_t n){
    void* d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(value) : "memory");
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, size_t n){
    void* d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, size_t n){
    if(dst <= src || (const uint8_t*)src + n <= (uint8_t*)dst){
        return memcpy(dst, src, n);
    }
    // Suprapunere cu destinatia dupa sursa: copiere de la coada spre cap
    void* d = (uint8_t*)dst + n - 1;
    const void* s = (const uint8_t*)src + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, size_t n){
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for(size_t i = 0; i < n; i++){
        if(x[i] != y[i]){
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}
