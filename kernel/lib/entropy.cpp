#include "lib/entropy.h"

namespace {

int rdrand_state = -1;          // -1 = necunoscut, 0 / 1 dupa primul apel (CPUID.1:ECX bit 30)
uint64_t fallback_state = 0;

bool rdrand64(uint64_t& out){
    for(int i = 0; i < 10; i++){
        uint64_t v;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if(ok){
            out = v;
            return true;
        }
    }
    return false;
}

uint64_t tsc_mix(){
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = __atomic_add_fetch(&fallback_state, 0x9E3779B97F4A7C15UL, __ATOMIC_RELAXED) ^ ((uint64_t)hi << 32 | lo);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9UL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBUL;
    return x ^ (x >> 31);
}

}

bool entropy_has_rdrand(){
    if(rdrand_state < 0){
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        rdrand_state = (c & (1u << 30)) ? 1 : 0;
    }
    return rdrand_state == 1;
}

uint64_t entropy_u64(){
    uint64_t v;
    if(entropy_has_rdrand() && rdrand64(v)){
        return v;
    }
    return tsc_mix();
}
