#include "time/tsc.h"

namespace {

void cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d){
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

}

bool tsc_invariant(){
    uint32_t a, b, c, d;
    cpuid(0x80000000, a, b, c, d);
    if(a < 0x80000007){
        return false;
    }
    cpuid(0x80000007, a, b, c, d);
    return d & (1 << 8);
}

bool cpu_hypervisor(){
    uint32_t a, b, c, d;
    cpuid(1, a, b, c, d);
    return c & (1u << 31);
}

uint64_t tsc_calibrate(void (*wait_ms)(uint32_t), uint32_t ms){
    uint64_t start = rdtsc();
    wait_ms(ms);
    return (rdtsc() - start) * 1000 / ms;
}
