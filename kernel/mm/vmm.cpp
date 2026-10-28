#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/buddy.h"
#include "arch/x86_64/mmio.h"
#include "boot/multiboot2.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "init.h"

extern "C" char _kernel_start[];
extern "C" char _rodata_start[];
extern "C" char _data_start[];
extern "C" char _kernel_end[];

uint64_t phys_map_base = KERNEL_VMA;
uint64_t direct_map_limit = PAGE_1G;
AddressSpace kernel_as;

namespace {

const uint64_t KERNEL_WINDOW = PAGE_1G;     // KERNEL_VMA + [0, 1 GiB), ca in boot.S
const uint32_t MSR_EFER = 0xC0000080;
const uint64_t EFER_NXE = 1UL << 11;
const uint64_t CR0_WP = 1UL << 16;
const uint64_t CR4_PGE = 1UL << 7;
const uint64_t KERNEL_RW = PTE_PRESENT | PTE_RW | PTE_GLOBAL;

uint64_t nx_bit = 0;            // PTE_NX doar daca CPU-ul are NX
uint64_t current_pml4 = 0;

void cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d){
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

uint64_t* table(uint64_t phys){
    return (uint64_t*)phys_to_virt(phys);
}

uint64_t alloc_table(){
    uint64_t phys = pmm_alloc_page();
    if(!phys){
        return 0;
    }
    uint64_t* t = table(phys);
    for(int i = 0; i < 512; i++){
        t[i] = 0;
    }
    return phys;
}

// Tabelul de nivel urmator din entry, creat la nevoie; nullptr daca lipseste memorie
uint64_t* next_level(uint64_t& entry, uint64_t flags){
    if(entry & PTE_PRESENT){
        if(entry & PTE_HUGE){
            panic("vmm: conflict cu o pagina mare (entry=%lx)", entry);
        }
        entry |= flags & PTE_USER;
        return table(entry & PTE_ADDR_MASK);
    }
    uint64_t phys = alloc_table();
    if(!phys){
        return nullptr;
    }
    entry = phys | PTE_PRESENT | PTE_RW | (flags & PTE_USER);
    return table(phys);
}

// Intrarea finala pentru virt si marimea paginii pe care o mapeaza; 0 daca nu e mapata
uint64_t walk(const AddressSpace& as, uint64_t virt, uint64_t& size){
    uint64_t e = table(as.pml4_phys)[pml4_index(virt)];
    if(!(e & PTE_PRESENT)){
        return 0;
    }
    e = table(e & PTE_ADDR_MASK)[pdpt_index(virt)];
    size = PAGE_1G;
    if(!(e & PTE_PRESENT) || (e & PTE_HUGE)){
        return e & PTE_PRESENT ? e : 0;
    }
    e = table(e & PTE_ADDR_MASK)[pd_index(virt)];
    size = PAGE_2M;
    if(!(e & PTE_PRESENT) || (e & PTE_HUGE)){
        return e & PTE_PRESENT ? e : 0;
    }
    e = table(e & PTE_ADDR_MASK)[pt_index(virt)];
    size = PAGE_4K;
    return e & PTE_PRESENT ? e : 0;
}

uint64_t section_flags(uint64_t phys){
    uint64_t text = (uint64_t)_kernel_start - KERNEL_VMA;
    uint64_t rodata = (uint64_t)_rodata_start - KERNEL_VMA;
    uint64_t data = (uint64_t)_data_start - KERNEL_VMA;
    if(phys >= text && phys < rodata){
        return PTE_PRESENT | PTE_GLOBAL;                    // RX
    }
    if(phys >= rodata && phys < data){
        return PTE_PRESENT | PTE_GLOBAL | nx_bit;           // R
    }
    return KERNEL_RW | nx_bit;                              // .data/.bss si restul ferestrei: RW NX
}

// Fereastra KERNEL_VMA + [0, 1 GiB): pagini 2 MiB, cu pagini 4 KiB doar unde e imaginea kernelului
bool map_kernel_window(){
    uint64_t kstart = (uint64_t)_kernel_start - KERNEL_VMA;
    uint64_t kend = (uint64_t)_kernel_end - KERNEL_VMA;
    for(uint64_t phys = 0; phys < KERNEL_WINDOW; phys += PAGE_2M){
        uint64_t virt = KERNEL_VMA + phys;
        bool has_kernel = phys < kend && kstart < phys + PAGE_2M;
        if(!has_kernel){
            if(!vmm_map(kernel_as, virt, phys, KERNEL_RW | nx_bit | PTE_HUGE)){
                return false;
            }
            continue;
        }
        for(uint64_t p = phys; p < phys + PAGE_2M; p += PAGE_4K){
            if(!vmm_map(kernel_as, KERNEL_VMA + p, p, section_flags(p))){
                return false;
            }
        }
    }
    return true;
}

uint64_t ram_top(){
    uint64_t top = 0;
    for(uint32_t i = 0; i < boot_info.mmap_count; i++){
        const MemRegion& r = boot_info.mmap[i];
        if(r.type == MB2_MMAP_AVAILABLE && r.base + r.length > top){
            top = r.base + r.length;
        }
    }
    return top;
}

// HHDM: pagini de 1 GiB direct in PDPT daca CPU-ul le are, altfel 2 MiB
bool map_hhdm(uint64_t top, bool gb_pages){
    if(!gb_pages){
        for(uint64_t phys = 0; phys < top; phys += PAGE_2M){
            if(!vmm_map(kernel_as, HHDM_BASE + phys, phys, KERNEL_RW | nx_bit | PTE_HUGE)){
                return false;
            }
        }
        return true;
    }
    uint64_t* pml4 = table(kernel_as.pml4_phys);
    for(uint64_t phys = 0; phys < top; phys += PAGE_1G){
        uint64_t virt = HHDM_BASE + phys;
        uint64_t* pdpt = next_level(pml4[pml4_index(virt)], 0);
        if(!pdpt){
            return false;
        }
        pdpt[pdpt_index(virt)] = phys | KERNEL_RW | nx_bit | PTE_HUGE;
    }
    return true;
}

}

__init void vmm_init(){
    uint32_t a, b, c, d;
    cpuid(0x80000001, a, b, c, d);
    bool has_nx = d & (1u << 20);
    bool gb_pages = d & (1u << 26);
    if(has_nx){
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
        lo |= EFER_NXE;
        __asm__ volatile("wrmsr" : : "c"(MSR_EFER), "a"(lo), "d"(hi));
        nx_bit = PTE_NX;
    }

    kernel_as.pml4_phys = alloc_table();
    uint64_t top = ram_top();
    uint64_t hhdm_size = gb_pages ? (top + PAGE_1G - 1) & ~(PAGE_1G - 1) : (top + PAGE_2M - 1) & ~(PAGE_2M - 1);
    if(!kernel_as.pml4_phys || !map_kernel_window() || !map_hhdm(hhdm_size, gb_pages)){
        panic("vmm_init: memorie insuficienta pentru tabele");
    }
    // Fereastra MMIO (ultimul GiB) ramane aceeasi ca in boot
    uint64_t* kpdpt = table(table(kernel_as.pml4_phys)[pml4_index(KERNEL_VMA)] & PTE_ADDR_MASK);
    kpdpt[pdpt_index(MMIO_WINDOW_BASE)] = mmio_pd_phys() | PTE_PRESENT | PTE_RW;

    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | CR4_PGE));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | CR0_WP));
    as_switch(kernel_as);

    phys_map_base = HHDM_BASE;
    direct_map_limit = hhdm_size;
    buddy_rebuild();

    kprintf("vmm: HHDM %lu MiB la %lx (pagini %s), NX %s\n", hhdm_size >> 20, HHDM_BASE,
            gb_pages ? "1 GiB" : "2 MiB", has_nx ? "da" : "nu");
    kprintf("[OK] vmm free=%lu MiB\n", pmm_free_count() * PAGE_SIZE >> 20);
}

bool vmm_map(AddressSpace& as, uint64_t virt, uint64_t phys, uint64_t flags){
    flags = (flags | PTE_PRESENT) & (nx_bit | ~PTE_NX);
    uint64_t* pml4 = table(as.pml4_phys);
    uint64_t* pdpt = next_level(pml4[pml4_index(virt)], flags);
    if(!pdpt){
        return false;
    }
    uint64_t* pd = next_level(pdpt[pdpt_index(virt)], flags);
    if(!pd){
        return false;
    }
    if(flags & PTE_HUGE){
        pd[pd_index(virt)] = (phys & ~(PAGE_2M - 1)) | flags;
    } else {
        uint64_t* pt = next_level(pd[pd_index(virt)], flags);
        if(!pt){
            return false;
        }
        pt[pt_index(virt)] = (phys & PTE_ADDR_MASK) | flags;
    }
    if(as.pml4_phys == current_pml4){
        invlpg(virt);
    }
    return true;
}

void vmm_unmap(AddressSpace& as, uint64_t virt){
    uint64_t e = table(as.pml4_phys)[pml4_index(virt)];
    if(!(e & PTE_PRESENT)){
        return;
    }
    uint64_t* pdpt = table(e & PTE_ADDR_MASK);
    uint64_t& pdpte = pdpt[pdpt_index(virt)];
    if(!(pdpte & PTE_PRESENT)){
        return;
    }
    if(pdpte & PTE_HUGE){
        pdpte = 0;
    } else {
        uint64_t* pd = table(pdpte & PTE_ADDR_MASK);
        uint64_t& pde = pd[pd_index(virt)];
        if(!(pde & PTE_PRESENT)){
            return;
        }
        if(pde & PTE_HUGE){
            pde = 0;
        } else {
            table(pde & PTE_ADDR_MASK)[pt_index(virt)] = 0;
        }
    }
    if(as.pml4_phys == current_pml4){
        invlpg(virt);
    }
}

uint64_t vmm_entry(const AddressSpace& as, uint64_t virt){
    uint64_t size;
    return walk(as, virt, size);
}

uint64_t vmm_translate(const AddressSpace& as, uint64_t virt){
    uint64_t size;
    uint64_t e = walk(as, virt, size);
    if(!e){
        return 0;
    }
    return (e & PTE_ADDR_MASK & ~(size - 1)) + (virt & (size - 1));
}

AddressSpace as_create(){
    AddressSpace as = {alloc_table()};
    if(as.pml4_phys){
        uint64_t* dst = table(as.pml4_phys);
        const uint64_t* src = table(kernel_as.pml4_phys);
        for(int i = 256; i < 512; i++){
            dst[i] = src[i];
        }
    }
    return as;
}

void as_switch(const AddressSpace& as){
    current_pml4 = as.pml4_phys;
    __asm__ volatile("mov %0, %%cr3" : : "r"(as.pml4_phys) : "memory");
}

bool as_copy_user(AddressSpace& dst, const AddressSpace& src){
    const uint64_t* pml4 = table(src.pml4_phys);
    for(int i = 0; i < 256; i++){
        if(!(pml4[i] & PTE_PRESENT)){
            continue;
        }
        const uint64_t* pdpt = table(pml4[i] & PTE_ADDR_MASK);
        for(int j = 0; j < 512; j++){
            if(!(pdpt[j] & PTE_PRESENT)){
                continue;
            }
            if(pdpt[j] & PTE_HUGE){
                panic("as_copy_user: pagina de 1 GiB in jumatatea user");
            }
            const uint64_t* pd = table(pdpt[j] & PTE_ADDR_MASK);
            for(int k = 0; k < 512; k++){
                if(!(pd[k] & PTE_PRESENT)){
                    continue;
                }
                if(pd[k] & PTE_HUGE){
                    panic("as_copy_user: pagina de 2 MiB in jumatatea user");
                }
                const uint64_t* pt = table(pd[k] & PTE_ADDR_MASK);
                for(int l = 0; l < 512; l++){
                    if(!(pt[l] & PTE_PRESENT)){
                        continue;
                    }
                    uint64_t virt = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    uint64_t phys = pmm_alloc_page();
                    if(!phys){
                        return false;
                    }
                    memcpy(phys_to_virt(phys), phys_to_virt(pt[l] & PTE_ADDR_MASK), PAGE_4K);
                    if(!vmm_map(dst, virt, phys, pt[l] & ~PTE_ADDR_MASK)){
                        pmm_free_page(phys);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

void as_destroy(AddressSpace& as){
    if(!as.pml4_phys || as.pml4_phys == kernel_as.pml4_phys || as.pml4_phys == current_pml4){
        panic("as_destroy: spatiu invalid sau activ (%lx)", as.pml4_phys);
    }
    uint64_t* pml4 = table(as.pml4_phys);
    for(int i = 0; i < 256; i++){
        if(!(pml4[i] & PTE_PRESENT)){
            continue;
        }
        uint64_t* pdpt = table(pml4[i] & PTE_ADDR_MASK);
        for(int j = 0; j < 512; j++){
            if(!(pdpt[j] & PTE_PRESENT)){
                continue;
            }
            if(pdpt[j] & PTE_HUGE){
                panic("as_destroy: pagina de 1 GiB in jumatatea user");
            }
            uint64_t* pd = table(pdpt[j] & PTE_ADDR_MASK);
            for(int k = 0; k < 512; k++){
                if(!(pd[k] & PTE_PRESENT)){
                    continue;
                }
                if(pd[k] & PTE_HUGE){
                    panic("as_destroy: pagina de 2 MiB in jumatatea user");
                }
                uint64_t* pt = table(pd[k] & PTE_ADDR_MASK);
                for(int l = 0; l < 512; l++){
                    if(pt[l] & PTE_PRESENT){
                        pmm_free_page(pt[l] & PTE_ADDR_MASK);
                    }
                }
                pmm_free_page(pd[k] & PTE_ADDR_MASK);
            }
            pmm_free_page(pdpt[j] & PTE_ADDR_MASK);
        }
        pmm_free_page(pml4[i] & PTE_ADDR_MASK);
    }
    pmm_free_page(as.pml4_phys);
    as.pml4_phys = 0;
}
