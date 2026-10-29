#include "arch/x86_64/pat.h"

namespace {

const uint32_t MSR_PAT = 0x277;
const uint32_t CPUID_PAT = 1u << 16;        // CPUID.1:EDX

// PA0 WB, PA1 WC, PA2 UC-, PA3 UC, PA4 WB, PA5 WT, PA6 UC-, PA7 UC (implicit: PA1 = WT)
const uint64_t PAT_VALUE = 0x0007040600070106UL;

}

bool pat_supported(){
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return d & CPUID_PAT;
}

bool pat_init_cpu(){
    if(!pat_supported()){
        return false;
    }
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_PAT));
    if((((uint64_t)hi << 32) | lo) == PAT_VALUE){
        return true;
    }
    // Schimbarea tipurilor de memorie: cache golit inainte si dupa, TLB-ul reincarcat
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    __asm__ volatile("wbinvd" : : : "memory");
    __asm__ volatile("wrmsr" : : "c"(MSR_PAT), "a"((uint32_t)PAT_VALUE), "d"((uint32_t)(PAT_VALUE >> 32)));
    __asm__ volatile("wbinvd" : : : "memory");
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    if(flags & (1UL << 9)){
        __asm__ volatile("sti" : : : "memory");
    }
    return true;
}
