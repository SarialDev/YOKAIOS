#pragma once
#include "types.h"

struct Task;

const int MAX_CPUS = 16;
const size_t PERCPU_IST_SIZE = 16384;

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

// Starea fiecarui CPU, la GS base in kernel (swapgs la intrarea din ring 3).
// Primele campuri au offset-uri fixe: syscall.S foloseste %gs:0 si %gs:8, this_cpu %gs:16, task_current %gs:24,
// cpu_id %gs:32
struct PerCpu {
    uint64_t kstack_top;            // stiva de kernel pentru syscall (= TSS.rsp0)
    uint64_t user_rsp;              // rsp-ul user salvat de syscall_entry
    PerCpu* self;
    Task* current;
    uint32_t id;                    // index logic 0..cpu_count-1 (0 = BSP)
    uint32_t lapic_id;
    volatile bool online;
    volatile bool need_resched;
    volatile bool kill_pending;     // page fault user: procesul moare la iesirea din exceptie
    volatile bool tlb_pending;      // TLB shootdown cerut de alt CPU
    Task* idle;
    Task* prev;                     // task-ul de pe care tocmai s-a comutat (eliberat dupa switch_to)
    uint64_t cr3;                   // spatiul de adrese incarcat acum (pentru TLB shootdown)
    uint64_t next_tick_ns;          // urmatorul tick de 1 ms al acestui CPU (timer one-shot)
    uint64_t ipis;                  // IPI-uri de reschedule primite
    uint8_t* ist1;                  // stiva pentru #DF si NMI
    uint64_t gdt[7] __attribute__((aligned(16)));
    Tss tss __attribute__((aligned(16)));
};
static_assert(__builtin_offsetof(PerCpu, kstack_top) == 0 && __builtin_offsetof(PerCpu, user_rsp) == 8 &&
              __builtin_offsetof(PerCpu, self) == 16 && __builtin_offsetof(PerCpu, current) == 24 &&
              __builtin_offsetof(PerCpu, id) == 32,
              "PerCpu trebuie sa corespunda cu syscall.S, this_cpu, task_current si cpu_id");

extern PerCpu cpus[MAX_CPUS];

// Primul lucru pe fiecare CPU: GS base = &cpus[id]
void percpu_init(uint32_t id, uint32_t lapic_id);

inline PerCpu* this_cpu(){
    PerCpu* c;
    __asm__ volatile("mov %%gs:16, %0" : "=r"(c));
    return c;
}

// CPU-uri pornite (1 pana la smp_init)
uint32_t smp_cpu_count();
