#include "arch/x86_64/fpu.h"
#include "lib/kprintf.h"

namespace {

alignas(16) uint8_t default_state[FPU_STATE_SIZE];

const uint64_t CR0_MP = 1UL << 1;
const uint64_t CR0_EM = 1UL << 2;
const uint64_t CR0_TS = 1UL << 3;
const uint64_t CR4_OSFXSR = 1UL << 9;
const uint64_t CR4_OSXMMEXCPT = 1UL << 10;

void load_default_regs(){
    uint32_t mxcsr = 0x1F80;                    // toate exceptiile mascate, rotunjire la cel mai apropiat
    __asm__ volatile("fninit" ::: "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

}

void fpu_init(){
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(CR0_EM | CR0_TS)) | CR0_MP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    load_default_regs();
    fpu_save(default_state);
    for(int i = 160; i < 416; i++){
        default_state[i] = 0;                   // XMM0..15: niciun proces nu mosteneste ce a lasat firmware-ul
    }
    kprintf("[OK] fpu (x87 + SSE, FXSAVE %lu bytes)\n", (unsigned long)FPU_STATE_SIZE);
}

void fpu_init_cpu(){
    load_default_regs();
}

void fpu_save(void* area){
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const void* area){
    __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

const void* fpu_default_state(){
    return default_state;
}

void fpu_reset(){
    fpu_restore(default_state);
}
