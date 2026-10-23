#pragma once
#include "interrupt.h"
#include "mm/vmm.h"

// Bitii din error code-ul #PF
const uint64_t PF_ERR_PRESENT = 1UL << 0;   // P: pagina era prezenta (violare de protectie)
const uint64_t PF_ERR_WRITE = 1UL << 1;     // W: scriere
const uint64_t PF_ERR_USER = 1UL << 2;      // U: din ring 3
const uint64_t PF_ERR_FETCH = 1UL << 4;     // I: fetch de instructiune

// Inregistreaza handlerul de #PF
void fault_init();

// Hook pentru memoria virtuala a proceselor (lazy alloc, CoW, stiva): true daca a rezolvat faultul
bool vma_fault(AddressSpace& as, uint64_t addr, uint64_t err);

// Fault nerecuperabil din user: procesul curent trebuie omorat (consumat de scheduler in T4.4)
bool fault_kill_pending();
void fault_kill_clear();

// Dupa init: elibereaza paginile .init.text si verifica protectiile sectiunilor kernelului
void fault_protect_kernel();
