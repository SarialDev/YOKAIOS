#include "tests/test_mm.h"
#include "interrupt.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "mm/buddy.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "mm/fault.h"

namespace {

const uint8_t VEC_PF = 14;
const int BUDDY_BLOCKS = 256;
const int BUDDY_TEST_MAX_ORDER = 6;
const int SLAB_OPS = 10000;
const int SLAB_SLOTS = 128;
const size_t SLAB_MAX_SIZE = 4096;          // peste 2048 trece prin buddy
const uint64_t TEST_VIRT = 0xFFFFA00000000000UL;     // in afara HHDM, a kernelului si a ferestrei MMIO

uint32_t seed = 0x1234567;

uint32_t rnd(){
    seed = seed * 1103515245 + 12345;
    return seed >> 8;
}

// Octetul i dintr-un bloc depinde de "id" si de pozitie, ca suprapunerile sa se vada
uint8_t pattern(uint32_t id, size_t i){
    return (uint8_t)(id * 31 + i * 7 + (i >> 8));
}

void fill(void* p, size_t n, uint32_t id){
    uint8_t* b = (uint8_t*)p;
    for(size_t i = 0; i < n; i++){
        b[i] = pattern(id, i);
    }
}

bool intact(const void* p, size_t n, uint32_t id){
    const uint8_t* b = (const uint8_t*)p;
    for(size_t i = 0; i < n; i++){
        if(b[i] != pattern(id, i)){
            return false;
        }
    }
    return true;
}

void test_buddy(){
    uint64_t phys[BUDDY_BLOCKS];
    int orders[BUDDY_BLOCKS];
    uint64_t free_before = buddy_free_count();
    uint64_t pages = 0;
    for(int i = 0; i < BUDDY_BLOCKS; i++){
        orders[i] = rnd() % (BUDDY_TEST_MAX_ORDER + 1);
        phys[i] = alloc_pages(orders[i]);
        if(!phys[i] || phys[i] % (PAGE_SIZE << orders[i])){
            panic("test_buddy: alloc_pages(%d) a intors %lx", orders[i], phys[i]);
        }
        pages += 1UL << orders[i];
        // Primul si ultimul cuvant din bloc: un bloc dat de doua ori s-ar suprascrie
        uint64_t* first = (uint64_t*)phys_to_virt(phys[i]);
        uint64_t* last = (uint64_t*)phys_to_virt(phys[i] + (PAGE_SIZE << orders[i]) - 8);
        *first = phys[i];
        *last = ~phys[i];
    }
    if(buddy_free_count() != free_before - pages){
        panic("test_buddy: free count %lu, asteptat %lu", buddy_free_count(), free_before - pages);
    }
    for(int i = 0; i < BUDDY_BLOCKS; i++){
        uint64_t* first = (uint64_t*)phys_to_virt(phys[i]);
        uint64_t* last = (uint64_t*)phys_to_virt(phys[i] + (PAGE_SIZE << orders[i]) - 8);
        if(*first != phys[i] || *last != ~phys[i]){
            panic("test_buddy: blocul %lx (ordin %d) a fost suprascris", phys[i], orders[i]);
        }
    }
    // Fisher-Yates: eliberare in ordine aleatoare
    for(int i = BUDDY_BLOCKS - 1; i > 0; i--){
        int j = rnd() % (i + 1);
        uint64_t p = phys[i]; phys[i] = phys[j]; phys[j] = p;
        int o = orders[i]; orders[i] = orders[j]; orders[j] = o;
    }
    for(int i = 0; i < BUDDY_BLOCKS; i++){
        free_pages(phys[i], orders[i]);
    }
    if(buddy_free_count() != free_before){
        panic("test_buddy: free count %lu dupa eliberare, asteptat %lu", buddy_free_count(), free_before);
    }
    kprintf("[OK] test_buddy (%d blocuri ordin 0-%d, %lu pagini)\n", BUDDY_BLOCKS, BUDDY_TEST_MAX_ORDER, pages);
}

void test_slab(){
    void* ptrs[SLAB_SLOTS] = {};
    size_t sizes[SLAB_SLOTS] = {};
    uint32_t ids[SLAB_SLOTS] = {};
    uint64_t free_before = pmm_free_count();
    for(int op = 0; op < SLAB_OPS; op++){
        int s = rnd() % SLAB_SLOTS;
        if(ptrs[s]){
            if(!intact(ptrs[s], sizes[s], ids[s])){
                panic("test_slab: obiect corupt la %p (%lu bytes, op %d)", ptrs[s], sizes[s], op);
            }
            kfree(ptrs[s]);
            ptrs[s] = nullptr;
            continue;
        }
        sizes[s] = 1 + rnd() % SLAB_MAX_SIZE;
        ids[s] = op;
        ptrs[s] = kmalloc(sizes[s]);
        if(!ptrs[s] || (uint64_t)ptrs[s] % 16){
            panic("test_slab: kmalloc(%lu) a intors %p", sizes[s], ptrs[s]);
        }
        fill(ptrs[s], sizes[s], ids[s]);
    }
    for(int s = 0; s < SLAB_SLOTS; s++){
        if(ptrs[s]){
            if(!intact(ptrs[s], sizes[s], ids[s])){
                panic("test_slab: obiect corupt la %p (%lu bytes)", ptrs[s], sizes[s]);
            }
            kfree(ptrs[s]);
        }
    }
    if(pmm_free_count() != free_before){
        panic("test_slab: %lu pagini libere dupa, %lu inainte", pmm_free_count(), free_before);
    }
    kprintf("[OK] test_slab (%d operatii, marimi 1-%lu)\n", SLAB_OPS, SLAB_MAX_SIZE);
}

volatile uint64_t resume_rip = 0;
volatile uint64_t seen_vector = ~0UL;
volatile uint64_t seen_error = 0;
volatile uint64_t seen_cr2 = 0;

void record_and_skip(InterruptFrame* f){
    seen_vector = f->vector;
    seen_error = f->error;
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    seen_cr2 = cr2;
    f->rip = resume_rip;
}

uint64_t read_after_unmap(uint64_t addr){
    uint64_t tmp, val = 0;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "mov (%3), %2\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip), "+r"(val) : "r"(addr) : "memory");
    return val;
}

void test_vmm(){
    uint64_t page = pmm_alloc_page();
    if(!page || !vmm_map(kernel_as, TEST_VIRT, page, PTE_RW | PTE_NX)){
        panic("test_vmm: pmm_alloc_page/vmm_map a esuat");
    }
    if(vmm_translate(kernel_as, TEST_VIRT + 0x123) != page + 0x123){
        panic("test_vmm: translate = %lx, asteptat %lx", vmm_translate(kernel_as, TEST_VIRT + 0x123), page + 0x123);
    }
    *(volatile uint64_t*)TEST_VIRT = 0x7E57;
    if(*(volatile uint64_t*)phys_to_virt(page) != 0x7E57){
        panic("test_vmm: scrierea prin maparea noua nu a ajuns in pagina fizica");
    }

    vmm_unmap(kernel_as, TEST_VIRT);
    if(vmm_translate(kernel_as, TEST_VIRT)){
        panic("test_vmm: adresa inca mapata dupa vmm_unmap");
    }
    InterruptHandler old_pf = irq_register(VEC_PF, record_and_skip);
    seen_vector = ~0UL;
    read_after_unmap(TEST_VIRT);
    irq_register(VEC_PF, old_pf);
    if(seen_vector != VEC_PF || seen_cr2 != TEST_VIRT || (seen_error & PF_ERR_PRESENT)){
        panic("test_vmm: acces dupa unmap fara #PF (vector=%lx cr2=%lx err=%lx)", seen_vector, seen_cr2, seen_error);
    }
    pmm_free_page(page);
    kprintf("[OK] test_vmm (map/translate/unmap la %lx, #PF dupa unmap)\n", TEST_VIRT);
}

}

void test_mm_run(){
    test_buddy();
    test_slab();
    test_vmm();
}
