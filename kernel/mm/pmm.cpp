#include "mm/pmm.h"
#include "boot/multiboot2.h"
#include "core/panic.h"
#include "lib/kprintf.h"

extern "C" char _kernel_start[];
extern "C" char _kernel_end[];

namespace {

const uint64_t LOW_MEMORY_END = 0x100000;       // sub 1 MiB: BIOS, EBDA, VGA
const uint64_t DIRECT_MAP_LIMIT = 1UL << 30;    // bitmap-ul trebuie sa fie in maparea de boot

uint64_t* bitmap = nullptr;
uint64_t total_pages = 0;
uint64_t bitmap_words = 0;
uint64_t free_pages = 0;
uint64_t next_word = 0;         // de aici incepe cautarea urmatoarei alocari

uint64_t align_up(uint64_t v){
    return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

uint64_t align_down(uint64_t v){
    return v & ~(PAGE_SIZE - 1);
}

bool test_bit(uint64_t page){
    return bitmap[page / 64] & (1UL << (page % 64));
}

void set_bit(uint64_t page){
    bitmap[page / 64] |= 1UL << (page % 64);
}

void clear_bit(uint64_t page){
    bitmap[page / 64] &= ~(1UL << (page % 64));
}

// Marcheaza ocupat [start, end) aliniat la pagini (extins spre exterior)
void reserve(uint64_t start, uint64_t end){
    uint64_t last = align_up(end) / PAGE_SIZE;
    if(last > total_pages){
        last = total_pages;
    }
    for(uint64_t p = align_down(start) / PAGE_SIZE; p < last; p++){
        if(!test_bit(p)){
            set_bit(p);
            free_pages--;
        }
    }
}

bool overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end){
    return a_start < b_end && b_start < a_end;
}

bool collides_with_boot_data(uint64_t start, uint64_t end){
    if(overlaps(start, end, (uint64_t)_kernel_start - KERNEL_VMA, (uint64_t)_kernel_end - KERNEL_VMA)){
        return true;
    }
    if(overlaps(start, end, boot_info.mbi_phys, boot_info.mbi_phys + boot_info.mbi_size)){
        return true;
    }
    for(uint32_t i = 0; i < boot_info.module_count; i++){
        if(overlaps(start, end, boot_info.modules[i].start, boot_info.modules[i].end)){
            return true;
        }
    }
    return false;
}

// Prima adresa dintr-o regiune libera unde incap `size` bytes fara sa atinga kernel/mbi/module
uint64_t place_bitmap(uint64_t size){
    for(uint32_t i = 0; i < boot_info.mmap_count; i++){
        const MemRegion& r = boot_info.mmap[i];
        if(r.type != MB2_MMAP_AVAILABLE){
            continue;
        }
        uint64_t end = align_down(r.base + r.length);
        uint64_t cand = align_up(r.base < LOW_MEMORY_END ? LOW_MEMORY_END : r.base);
        while(cand + size <= end && cand + size <= DIRECT_MAP_LIMIT){
            if(!collides_with_boot_data(cand, cand + size)){
                return cand;
            }
            cand += PAGE_SIZE;
        }
    }
    return 0;
}

}

void pmm_init(){
    uint64_t top = 0;
    for(uint32_t i = 0; i < boot_info.mmap_count; i++){
        const MemRegion& r = boot_info.mmap[i];
        if(r.type == MB2_MMAP_AVAILABLE && r.base + r.length > top){
            top = r.base + r.length;
        }
    }
    total_pages = top / PAGE_SIZE;
    bitmap_words = (total_pages + 63) / 64;
    uint64_t bitmap_size = align_up(bitmap_words * 8);

    uint64_t bitmap_phys = place_bitmap(bitmap_size);
    if(!bitmap_phys){
        panic("pmm: nu gasesc loc pentru bitmap (%lu bytes)", bitmap_size);
    }
    bitmap = (uint64_t*)(KERNEL_VMA + bitmap_phys);

    // Totul ocupat, apoi eliberam doar paginile intregi din regiunile disponibile
    for(uint64_t i = 0; i < bitmap_words; i++){
        bitmap[i] = ~0UL;
    }
    for(uint32_t i = 0; i < boot_info.mmap_count; i++){
        const MemRegion& r = boot_info.mmap[i];
        if(r.type != MB2_MMAP_AVAILABLE){
            continue;
        }
        for(uint64_t p = align_up(r.base) / PAGE_SIZE; p < align_down(r.base + r.length) / PAGE_SIZE; p++){
            if(test_bit(p)){
                clear_bit(p);
                free_pages++;
            }
        }
    }

    reserve(0, LOW_MEMORY_END);
    reserve((uint64_t)_kernel_start - KERNEL_VMA, (uint64_t)_kernel_end - KERNEL_VMA);
    reserve(boot_info.mbi_phys, boot_info.mbi_phys + boot_info.mbi_size);
    for(uint32_t i = 0; i < boot_info.module_count; i++){
        reserve(boot_info.modules[i].start, boot_info.modules[i].end);
    }
    reserve(bitmap_phys, bitmap_phys + bitmap_size);

    kprintf("pmm: %lu pagini, bitmap %lu KiB la %lx, kernel %lu KiB\n", total_pages, bitmap_size >> 10,
            bitmap_phys, (uint64_t)(_kernel_end - _kernel_start) >> 10);
    kprintf("[OK] pmm free=%lu MiB\n", free_pages * PAGE_SIZE >> 20);
}

uint64_t pmm_alloc_page(){
    for(uint64_t n = 0; n < bitmap_words; n++){
        uint64_t w = (next_word + n) % bitmap_words;
        if(bitmap[w] == ~0UL){
            continue;
        }
        uint64_t page = w * 64 + __builtin_ctzl(~bitmap[w]);
        if(page >= total_pages){
            continue;
        }
        set_bit(page);
        free_pages--;
        next_word = w;
        return page * PAGE_SIZE;
    }
    return 0;
}

void pmm_free_page(uint64_t phys){
    uint64_t page = phys / PAGE_SIZE;
    if(phys % PAGE_SIZE || page >= total_pages){
        panic("pmm_free_page: adresa invalida %lx", phys);
    }
    if(!test_bit(page)){
        panic("pmm_free_page: double free la %lx", phys);
    }
    clear_bit(page);
    free_pages++;
    if(page / 64 < next_word){
        next_word = page / 64;
    }
}

uint64_t pmm_alloc_contig(uint64_t n){
    if(n == 0){
        return 0;
    }
    uint64_t run = 0;
    for(uint64_t p = 0; p < total_pages; p++){
        if(test_bit(p)){
            run = 0;
            continue;
        }
        if(++run == n){
            uint64_t first = p + 1 - n;
            for(uint64_t q = first; q <= p; q++){
                set_bit(q);
            }
            free_pages -= n;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_free_count(){
    return free_pages;
}
