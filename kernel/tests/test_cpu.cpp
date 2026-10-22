#include "tests/test_cpu.h"
#include "interrupt.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "time/clock.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

namespace {

const uint64_t VEC_DE = 0;
const uint64_t VEC_PF = 14;
const uint64_t UNMAPPED_ADDR = 0x1000;     // harta identitate 0-1 GiB e stearsa in boot.S
const int MONOTONIC_READS = 10000;
const uint32_t SLEEP_MS = 100;
const uint64_t SLEEP_MIN_NS = 95000000;
const uint64_t SLEEP_MAX_NS = 105000000;
const uint64_t PF_PRESENT = 1 << 0;
const uint64_t PF_WRITE = 1 << 1;
const uint64_t PF_FETCH = 1 << 4;
const uint64_t TEST_USER_VIRT = 0x400000;

// Un singur ret intr-o pagina RW+NX (.data): executia lui trebuie sa dea #PF
volatile uint8_t data_code[16] = {0xC3};

volatile uint64_t resume_rip = 0;
volatile uint64_t seen_vector = ~0UL;
volatile uint64_t seen_error = 0;
volatile uint64_t seen_cr2 = 0;

// Inregistreaza exceptia si sare peste instructiunea care a generat-o
void record_and_skip(InterruptFrame* f){
    seen_vector = f->vector;
    seen_error = f->error;
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    seen_cr2 = cr2;
    f->rip = resume_rip;
}

void reset(){
    seen_vector = ~0UL;
    seen_error = 0;
    seen_cr2 = 0;
}

void trigger_de(){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "xor %%ecx, %%ecx\n\t"
        "mov $1, %%eax\n\t"
        "xor %%edx, %%edx\n\t"
        "div %%ecx\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : : "eax", "ecx", "edx", "memory");
}

void trigger_pf(){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "mov (%2), %%eax\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : "r"(UNMAPPED_ADDR) : "eax", "memory");
}

void trigger_text_write(uint64_t addr){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "movb $0x90, (%2)\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : "r"(addr) : "memory");
}

// jmp (nu call) ca stiva sa ramana neschimbata cand handlerul sare la eticheta
void trigger_nx_exec(uint64_t addr){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "jmp *%2\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : "r"(addr) : "memory");
}

void test_exceptions(){
    irq_register(VEC_DE, record_and_skip);
    irq_register(VEC_PF, record_and_skip);

    reset();
    trigger_de();
    if(seen_vector != VEC_DE){
        panic("test_exceptions: #DE asteptat, vector=%lx", seen_vector);
    }

    reset();
    trigger_pf();
    if(seen_vector != VEC_PF || seen_cr2 != UNMAPPED_ADDR){
        panic("test_exceptions: #PF asteptat, vector=%lx cr2=%lx", seen_vector, seen_cr2);
    }

    irq_register(VEC_DE, nullptr);
    irq_register(VEC_PF, nullptr);
    kprintf("[OK] test_exceptions\n");
}

void test_paging(){
    irq_register(VEC_PF, record_and_skip);

    uint64_t text = (uint64_t)&test_cpu_run;
    reset();
    trigger_text_write(text);
    if(seen_vector != VEC_PF || seen_cr2 != text || (seen_error & (PF_PRESENT | PF_WRITE)) != (PF_PRESENT | PF_WRITE)){
        panic("test_paging: scriere in .text fara #PF (vector=%lx err=%lx)", seen_vector, seen_error);
    }

    uint64_t data = (uint64_t)data_code;
    reset();
    trigger_nx_exec(data);
    if(seen_vector != VEC_PF || seen_cr2 != data || !(seen_error & PF_FETCH)){
        panic("test_paging: executie din .data fara #PF (vector=%lx err=%lx)", seen_vector, seen_error);
    }
    irq_register(VEC_PF, nullptr);

    // Spatiu de adrese nou: pagina proprie jos, kernelul vizibil sus
    AddressSpace as = as_create();
    uint64_t page = pmm_alloc_page();
    if(!as.pml4_phys || !page || !vmm_map(as, TEST_USER_VIRT, page, PTE_RW | PTE_USER | PTE_NX)){
        panic("test_paging: as_create/vmm_map a esuat");
    }
    as_switch(as);
    *(volatile uint64_t*)TEST_USER_VIRT = 0x59AB;
    as_switch(kernel_as);
    uint64_t seen = *(volatile uint64_t*)phys_to_virt(page);
    if(seen != 0x59AB || vmm_translate(as, TEST_USER_VIRT) != page || vmm_translate(kernel_as, TEST_USER_VIRT)){
        panic("test_paging: spatiul de adrese nou nu e izolat (val=%lx)", seen);
    }
    vmm_unmap(as, TEST_USER_VIRT);
    if(vmm_translate(as, TEST_USER_VIRT)){
        panic("test_paging: vmm_unmap nu a sters maparea");
    }
    pmm_free_page(page);
    kprintf("[OK] test_paging\n");
}

void test_timer(){
    __asm__ volatile("sti");
    uint64_t prev = monotonic_ns();
    for(int i = 0; i < MONOTONIC_READS; i++){
        uint64_t now = monotonic_ns();
        if(now <= prev){
            panic("test_timer: monotonic_ns nu creste la citirea %d (%lu <= %lu)", i, now, prev);
        }
        prev = now;
    }

    uint64_t start = monotonic_ns();
    sleep_ms(SLEEP_MS);
    uint64_t slept = monotonic_ns() - start;
    __asm__ volatile("cli");
    if(slept < SLEEP_MIN_NS || slept > SLEEP_MAX_NS){
        panic("test_timer: sleep_ms(%u) a durat %lu ns", SLEEP_MS, slept);
    }
    kprintf("[OK] test_timer (sleep_ms(%u) = %lu us)\n", SLEEP_MS, slept / 1000);
}

}

void test_cpu_run(){
    test_exceptions();
    test_paging();
    test_timer();
}
