#pragma once
#include "types.h"

// Intrari de tabel de pagini x86-64 (PML4 / PDPT / PD / PT)
const uint64_t PTE_PRESENT = 1UL << 0;
const uint64_t PTE_RW = 1UL << 1;
const uint64_t PTE_USER = 1UL << 2;
const uint64_t PTE_PWT = 1UL << 3;
const uint64_t PTE_PCD = 1UL << 4;
const uint64_t PTE_HUGE = 1UL << 7;     // 2 MiB in PD, 1 GiB in PDPT
const uint64_t PTE_GLOBAL = 1UL << 8;
const uint64_t PTE_NX = 1UL << 63;
const uint64_t PTE_ADDR_MASK = 0x000FFFFFFFFFF000UL;

const uint64_t PAGE_4K = 1UL << 12;
const uint64_t PAGE_2M = 1UL << 21;
const uint64_t PAGE_1G = 1UL << 30;

// Direct map al intregii RAM (HHDM)
const uint64_t HHDM_BASE = 0xFFFF800000000000UL;

// Baza prin care kernelul vede memoria fizica: KERNEL_VMA (primul GiB, din boot.S)
// pana la vmm_init, apoi HHDM_BASE. direct_map_limit = cata memorie fizica acopera
extern uint64_t phys_map_base;
extern uint64_t direct_map_limit;

inline void* phys_to_virt(uint64_t phys){
    return (void*)(phys_map_base + phys);
}

inline uint64_t pml4_index(uint64_t virt){ return (virt >> 39) & 511; }
inline uint64_t pdpt_index(uint64_t virt){ return (virt >> 30) & 511; }
inline uint64_t pd_index(uint64_t virt){ return (virt >> 21) & 511; }
inline uint64_t pt_index(uint64_t virt){ return (virt >> 12) & 511; }

inline void invlpg(uint64_t virt){
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
