#include "mm/pageref.h"
#include "mm/pmm.h"
#include "arch/x86_64/paging.h"
#include "core/panic.h"
#include "lib/string.h"

namespace {

// Un chunk acopera 2^18 pagini (1 GiB) cu 512 KiB de contoare
const uint64_t CHUNK_SHIFT = 18;
const uint64_t CHUNK_PAGES = 1UL << CHUNK_SHIFT;
const int MAX_CHUNKS = 64;

uint16_t* chunks[MAX_CHUNKS];
uint64_t total = 0;

uint16_t* slot(uint64_t phys){
    uint64_t page = phys / PAGE_SIZE;
    if(page >= total){
        panic("pageref: pagina %lx in afara RAM-ului", phys);
    }
    return &chunks[page >> CHUNK_SHIFT][page & (CHUNK_PAGES - 1)];
}

}

void pageref_init(){
    total = pmm_total_pages();
    uint64_t n = (total + CHUNK_PAGES - 1) >> CHUNK_SHIFT;
    if(n > (uint64_t)MAX_CHUNKS){
        panic("pageref: prea multa memorie (%lu pagini)", total);
    }
    for(uint64_t i = 0; i < n; i++){
        uint64_t pages_here = total - (i << CHUNK_SHIFT);
        if(pages_here > CHUNK_PAGES){
            pages_here = CHUNK_PAGES;
        }
        uint64_t bytes = (pages_here * sizeof(uint16_t) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t phys = pmm_alloc_contig(bytes / PAGE_SIZE);
        if(!phys){
            panic("pageref: memorie insuficienta pentru contoare");
        }
        chunks[i] = (uint16_t*)phys_to_virt(phys);
        memset(chunks[i], 0, bytes);
    }
}

void page_ref(uint64_t phys){
    uint16_t* c = slot(phys);
    if(__atomic_fetch_add(c, 1, __ATOMIC_ACQ_REL) == 0xFFFF){
        panic("pageref: prea multe referinte la %lx", phys);
    }
}

void page_put(uint64_t phys){
    uint16_t* c = slot(phys);
    uint16_t v = __atomic_load_n(c, __ATOMIC_ACQUIRE);
    for(;;){
        if(v == 0){
            pmm_free_page(phys);
            return;
        }
        if(__atomic_compare_exchange_n(c, &v, (uint16_t)(v - 1), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
            return;
        }
    }
}

bool page_shared(uint64_t phys){
    return __atomic_load_n(slot(phys), __ATOMIC_ACQUIRE) != 0;
}
