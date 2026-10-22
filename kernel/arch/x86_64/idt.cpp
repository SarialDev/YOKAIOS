#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/lapic.h"
#include "core/panic.h"

namespace {

struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};
static_assert(sizeof(IdtEntry) == 16, "intrare IDT 64-bit are 16 bytes");

struct __attribute__((packed)) IdtPtr {
    uint16_t limit;
    uint64_t base;
};

IdtEntry idt[256] __attribute__((aligned(16)));
IdtPtr idt_ptr;
InterruptHandler handlers[256];

const uint8_t GATE_INTERRUPT = 0x8E;   // prezent, DPL 0, interrupt gate 64-bit
const uint8_t GATE_USER = 0xEE;        // prezent, DPL 3

void set_gate(uint8_t vec, uint64_t addr, uint8_t ist, uint8_t type_attr){
    IdtEntry& e = idt[vec];
    e.offset_low = addr & 0xFFFF;
    e.selector = GDT_KCODE;
    e.ist = ist;
    e.type_attr = type_attr;
    e.offset_mid = (addr >> 16) & 0xFFFF;
    e.offset_high = addr >> 32;
    e.zero = 0;
}

}

extern "C" const uint64_t isr_stub_table[256];

void irq_register(uint8_t vec, InterruptHandler h){
    handlers[vec] = h;
}

extern "C" void interrupt_dispatch(InterruptFrame* f){
    InterruptHandler h = handlers[f->vector];
    if(h){
        h(f);
    } else if(f->vector < 32){
        panic_frame(f, exception_name(f->vector));
    }
    if(f->vector >= IDT_VEC_IRQ_FIRST && f->vector < IDT_VEC_IRQ_END){
        if(lapic_enabled()){
            lapic_eoi();
        } else if(f->vector < PIC_IRQ_BASE + PIC_IRQ_COUNT){
            pic_eoi(f->vector - PIC_IRQ_BASE);
        }
    }
}

void idt_init(){
    for(int v = 0; v < 256; v++){
        uint8_t ist = (v == IDT_VEC_NMI || v == IDT_VEC_DF) ? 1 : 0;
        uint8_t type = v == IDT_VEC_SYSCALL ? GATE_USER : GATE_INTERRUPT;
        set_gate((uint8_t)v, isr_stub_table[v], ist, type);
    }
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}
