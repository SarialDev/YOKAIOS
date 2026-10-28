#pragma once
#include "types.h"
#include "arch/x86_64/percpu.h"

// Vectori LAPIC (0x30-0x3F primesc EOI si trec prin sched_irq_exit)
const uint8_t IPI_TLB_VECTOR = 0x3D;
const uint8_t IPI_RESCHED_VECTOR = 0x3E;

// Porneste AP-urile din MADT cu INIT-SIPI-SIPI (dupa clock_init): fiecare primeste GDT/TSS, LAPIC timer
// si task idle proprii. Afiseaza "[OK] smp cpus=N"
void smp_init();

// CPU-ul cpu trebuie sa ruleze scheduler-ul (need_resched e deja setat)
void smp_send_resched(uint32_t cpu);

// Invalideaza virt in TLB-ul celorlalte CPU-uri care pot avea spatiul pml4_phys incarcat (toate, pentru
// jumatatea kernel) si asteapta confirmarea; intoarce cate CPU-uri au confirmat
uint32_t tlb_shootdown(uint64_t pml4_phys, uint64_t virt);

// Din buclele de asteptare cu intreruperile oprite (spinlock): rezolva un TLB shootdown cerut acestui CPU
void smp_spin_poll();

// Panic: opreste celelalte CPU-uri cu NMI
void smp_stop_others();
