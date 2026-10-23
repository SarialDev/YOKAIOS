#pragma once
#include "arch/x86_64/paging.h"

struct AddressSpace {
    uint64_t pml4_phys;
};

extern AddressSpace kernel_as;

// Construieste tabelele kernelului (HHDM + kernel pe sectiuni), comuta CR3 si extinde buddy
void vmm_init();

// Mapare 4 KiB (sau 2 MiB cu PTE_HUGE); PTE_PRESENT se adauga automat. false daca lipseste memorie
bool vmm_map(AddressSpace& as, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(AddressSpace& as, uint64_t virt);
// Adresa fizica pentru virt, 0 daca nu e mapata
uint64_t vmm_translate(const AddressSpace& as, uint64_t virt);
// Intrarea finala (PTE, sau PDE/PDPTE pentru pagini mari) cu tot cu flaguri, 0 daca nu e mapata
uint64_t vmm_entry(const AddressSpace& as, uint64_t virt);

// Spatiu nou cu jumatatea inferioara goala si jumatatea superioara (kernel) copiata; pml4_phys 0 la eroare
AddressSpace as_create();
void as_switch(const AddressSpace& as);
