#include "arch/x86_64/mmio.h"
#include "core/panic.h"

extern "C" uint64_t boot_pdpt[512];

namespace {

const uint64_t PAGE_2M = 2UL * 1024 * 1024;
const uint64_t PDE_LARGE_UNCACHED = 0x9B;   // P | RW | PWT | PCD | PS
const uint64_t PDE_ADDR_MASK = 0x000FFFFFFFE00000UL;

uint64_t mmio_pd[512] __attribute__((aligned(4096)));
int used_slots = 0;

// Cauta o secventa deja mapata care incepe cu pagina first
int find_mapped(uint64_t first, int pages){
    for(int s = 0; s + pages <= used_slots; s++){
        int i = 0;
        while(i < pages && (mmio_pd[s + i] & PDE_ADDR_MASK) == first + i * PAGE_2M){
            i++;
        }
        if(i == pages){
            return s;
        }
    }
    return -1;
}

}

uint64_t mmio_pd_phys(){
    return (uint64_t)mmio_pd - KERNEL_VMA;
}

void* mmio_map(uint64_t phys, uint64_t size){
    // Inainte de vmm_init fereastra se leaga in tabelele din boot.S
    if(!boot_pdpt[511]){
        boot_pdpt[511] = mmio_pd_phys() | 3;
    }
    uint64_t first = phys & ~(PAGE_2M - 1);
    int pages = (int)((phys + size - first + PAGE_2M - 1) / PAGE_2M);

    int slot = find_mapped(first, pages);
    if(slot < 0){
        if(used_slots + pages > 512){
            panic("mmio_map: fereastra MMIO plina (phys=%lx)", phys);
        }
        slot = used_slots;
        for(int i = 0; i < pages; i++){
            uint64_t virt = MMIO_WINDOW_BASE + (slot + i) * PAGE_2M;
            mmio_pd[slot + i] = (first + i * PAGE_2M) | PDE_LARGE_UNCACHED;
            __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        }
        used_slots += pages;
    }
    return (void*)(MMIO_WINDOW_BASE + slot * PAGE_2M + (phys - first));
}
