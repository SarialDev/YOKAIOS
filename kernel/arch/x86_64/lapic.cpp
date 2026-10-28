#include "arch/x86_64/lapic.h"
#include "arch/x86_64/mmio.h"

namespace {

const uint32_t REG_ID = 0x20;
const uint32_t REG_TPR = 0x80;
const uint32_t REG_EOI = 0xB0;
const uint32_t REG_SVR = 0xF0;
const uint32_t SVR_ENABLE = 1 << 8;
const uint32_t REG_LVT_TIMER = 0x320;
const uint32_t REG_TIMER_INIT = 0x380;
const uint32_t REG_TIMER_CURRENT = 0x390;
const uint32_t REG_TIMER_DIVIDE = 0x3E0;
const uint32_t REG_ICR_LOW = 0x300;
const uint32_t REG_ICR_HIGH = 0x310;
const uint32_t ICR_PENDING = 1 << 12;
const uint32_t ICR_ASSERT = 1 << 14;
const uint32_t ICR_LEVEL = 1 << 15;
const uint32_t ICR_NMI = 4 << 8;
const uint32_t ICR_INIT = 5 << 8;
const uint32_t ICR_STARTUP = 6 << 8;
const uint32_t ICR_ALL_BUT_SELF = 3 << 18;
const uint32_t LVT_MASKED = 1 << 16;
const uint32_t LVT_PERIODIC = 1 << 17;
const uint32_t DIVIDE_BY_16 = 0x3;

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

// Fiecare CPU are LAPIC-ul lui la aceeasi adresa fizica, deci aceeasi mapare
void enable_local(){
    wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE);
    write(REG_TPR, 0);
    write(REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

void wait_icr(){
    while(read(REG_ICR_LOW) & ICR_PENDING){
        __asm__ volatile("pause");
    }
}

// ICR are doua registre: o intrerupere intre ele care trimite si ea un IPI ar strica destinatia
void send_icr(uint32_t dest, uint32_t low){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    wait_icr();
    write(REG_ICR_HIGH, dest << 24);
    write(REG_ICR_LOW, low);
    wait_icr();
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

}

void lapic_init(uint64_t phys){
    lapic = (volatile uint32_t*)mmio_map(phys, 0x1000);
    enable_local();
}

void lapic_init_ap(){
    enable_local();
}

void lapic_send_ipi(uint32_t dest, uint8_t vector){
    send_icr(dest, ICR_ASSERT | vector);
}

void lapic_send_init(uint32_t dest){
    send_icr(dest, ICR_INIT | ICR_ASSERT | ICR_LEVEL);
}

void lapic_send_startup(uint32_t dest, uint8_t page){
    send_icr(dest, ICR_STARTUP | ICR_ASSERT | page);
}

void lapic_send_nmi_others(){
    send_icr(0, ICR_NMI | ICR_ASSERT | ICR_ALL_BUT_SELF);
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

uint32_t lapic_timer_measure(void (*wait_ms)(uint32_t), uint32_t ms){
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, LVT_MASKED);
    write(REG_TIMER_INIT, 0xFFFFFFFF);
    wait_ms(ms);
    uint32_t elapsed = 0xFFFFFFFF - read(REG_TIMER_CURRENT);
    write(REG_TIMER_INIT, 0);
    return elapsed;
}

void lapic_timer_periodic(uint32_t ticks, uint8_t vector){
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, LVT_PERIODIC | vector);
    write(REG_TIMER_INIT, ticks);
}

void lapic_timer_oneshot(uint8_t vector){
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, vector);
}

void lapic_timer_arm(uint32_t ticks){
    write(REG_TIMER_INIT, ticks ? ticks : 1);
}

void lapic_timer_stop(){
    write(REG_TIMER_INIT, 0);
}
