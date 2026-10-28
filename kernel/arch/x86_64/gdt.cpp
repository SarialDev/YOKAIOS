#include "arch/x86_64/gdt.h"
#include "arch/x86_64/percpu.h"

namespace {

struct __attribute__((packed)) GdtPtr {
    uint16_t limit;
    uint64_t base;
};

const uint32_t MSR_GS_BASE = 0xC0000101;

}

extern "C" void gdt_flush(const GdtPtr* ptr);

void gdt_init_cpu(PerCpu& c){
    uint64_t* gdt = c.gdt;
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFF;    // kernel code 64-bit
    gdt[2] = 0x00CF92000000FFFF;    // kernel data
    gdt[3] = 0x00CFF2000000FFFF;    // user data, DPL 3
    gdt[4] = 0x00AFFA000000FFFF;    // user code 64-bit, DPL 3

    Tss& tss = c.tss;
    tss = Tss{};
    tss.rsp[0] = c.kstack_top;
    tss.ist[0] = (uint64_t)(c.ist1 + PERCPU_IST_SIZE);
    tss.iomap_base = sizeof(Tss);

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(Tss) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)                    // prezent, TSS 64-bit disponibil
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    GdtPtr ptr = {sizeof(c.gdt) - 1, (uint64_t)gdt};
    gdt_flush(&ptr);
    // Unele CPU-uri sterg baza GS la incarcarea selectorului; percpu ramane valid
    uint64_t self = (uint64_t)&c;
    __asm__ volatile("wrmsr" : : "c"(MSR_GS_BASE), "a"((uint32_t)self), "d"((uint32_t)(self >> 32)));
}

void tss_set_rsp0(uint64_t rsp0){
    this_cpu()->tss.rsp[0] = rsp0;
}
