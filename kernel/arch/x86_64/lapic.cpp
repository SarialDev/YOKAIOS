#include "arch/x86_64/lapic.h"
#include "arch/x86_64/mmio.h"

namespace {

const uint32_t REG_ID = 0x20;
const uint32_t REG_TPR = 0x80;
const uint32_t REG_EOI = 0xB0;
const uint32_t REG_SVR = 0xF0;
const uint32_t SVR_ENABLE = 1 << 8;

const uint32_t MSR_APIC_BASE = 0x1B;
const uint64_t APIC_BASE_ENABLE = 1 << 11;

volatile uint32_t* lapic = nullptr;

uint32_t read(uint32_t reg){
    return lapic[reg / 4];
}

void write(uint32_t reg, uint32_t v){
    lapic[reg / 4] = v;
}

uint64_t rdmsr(uint32_t msr){
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void wrmsr(uint32_t msr, uint64_t v){
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

}

void lapic_init(uint64_t phys){
    wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE);
    lapic = (volatile uint32_t*)mmio_map(phys, 0x1000);
    write(REG_TPR, 0);
    write(REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

bool lapic_enabled(){
    return lapic != nullptr;
}

void lapic_eoi(){
    write(REG_EOI, 0);
}

uint32_t lapic_id(){
    return read(REG_ID) >> 24;
}
