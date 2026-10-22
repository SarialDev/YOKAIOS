#include "arch/x86_64/gdt.h"

namespace {

struct __attribute__((packed)) Tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
static_assert(sizeof(Tss) == 104, "TSS 64-bit are 104 bytes");

struct __attribute__((packed)) GdtPtr {
    uint16_t limit;
    uint64_t base;
};

// null, kcode, kdata, udata, ucode, TSS (2 intrari)
uint64_t gdt[7] __attribute__((aligned(16)));
Tss tss __attribute__((aligned(16)));
GdtPtr gdt_ptr;

// IST1: stiva separata pentru #DF si NMI
uint8_t ist1_stack[16384] __attribute__((aligned(16)));

}

extern "C" void gdt_flush(const GdtPtr* ptr);
extern "C" uint8_t stack_top[];

void gdt_init(){
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFF;    // kernel code 64-bit
    gdt[2] = 0x00CF92000000FFFF;    // kernel data
    gdt[3] = 0x00CFF2000000FFFF;    // user data, DPL 3
    gdt[4] = 0x00AFFA000000FFFF;    // user code 64-bit, DPL 3

    tss.rsp[0] = (uint64_t)stack_top;
    tss.ist[0] = (uint64_t)(ist1_stack + sizeof(ist1_stack));
    tss.iomap_base = sizeof(Tss);

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(Tss) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)                    // prezent, TSS 64-bit disponibil
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)gdt;
    gdt_flush(&gdt_ptr);
}

void tss_set_rsp0(uint64_t rsp0){
    tss.rsp[0] = rsp0;
}
