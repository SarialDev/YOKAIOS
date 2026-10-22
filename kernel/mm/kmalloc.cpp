#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "mm/buddy.h"
#include "mm/pmm.h"
#include "arch/x86_64/paging.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace {

const int CACHE_COUNT = 8;
const size_t CACHE_SIZES[CACHE_COUNT] = {16, 32, 64, 128, 256, 512, 1024, 2048};
const char* const CACHE_NAMES[CACHE_COUNT] = {
    "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048",
};
const uint32_t BIG_MAGIC = 0xB16A110C;

// Antet la inceputul unui bloc mare din buddy; pointerul intors e imediat dupa el
struct BigHeader {
    uint32_t magic;
    uint32_t order;
    uint64_t reserved;          // pastreaza alinierea la 16 a datelor
};
static_assert(sizeof(BigHeader) == 16, "BigHeader trebuie sa pastreze alinierea la 16");

KmemCache* caches[CACHE_COUNT];

void* big_alloc(size_t size){
    size_t total = size + sizeof(BigHeader);
    int order = 0;
    while((PAGE_SIZE << order) < total){
        if(++order > BUDDY_MAX_ORDER){
            return nullptr;
        }
    }
    uint64_t phys = alloc_pages(order);
    if(!phys){
        return nullptr;
    }
    BigHeader* h = (BigHeader*)phys_to_virt(phys);
    h->magic = BIG_MAGIC;
    h->order = order;
    return h + 1;
}

BigHeader* big_header_of(const void* ptr){
    BigHeader* h = (BigHeader*)((uint64_t)ptr & ~(PAGE_SIZE - 1));
    return h->magic == BIG_MAGIC && (const void*)(h + 1) == ptr ? h : nullptr;
}

size_t usable_size(const void* ptr){
    if(KmemCache* c = slab_cache_of(ptr)){
        return kmem_cache_object_size(c);
    }
    if(BigHeader* h = big_header_of(ptr)){
        return (PAGE_SIZE << h->order) - sizeof(BigHeader);
    }
    panic("kmalloc: pointer necunoscut %p", ptr);
}

}

void kmalloc_init(){
    for(int i = 0; i < CACHE_COUNT; i++){
        caches[i] = kmem_cache_create(CACHE_NAMES[i], CACHE_SIZES[i], 16);
        if(!caches[i]){
            panic("kmalloc_init: nu pot crea %s", CACHE_NAMES[i]);
        }
    }
    kprintf("[OK] heap (slab 16..2048, peste 2048 din buddy)\n");
}

void* kmalloc(size_t size){
    if(size == 0){
        return nullptr;
    }
    for(int i = 0; i < CACHE_COUNT; i++){
        if(size <= CACHE_SIZES[i]){
            return kmem_cache_alloc(caches[i]);
        }
    }
    return big_alloc(size);
}

void* kzalloc(size_t size){
    void* p = kmalloc(size);
    if(p){
        memset(p, 0, size);
    }
    return p;
}

void* krealloc(void* ptr, size_t size){
    if(!ptr){
        return kmalloc(size);
    }
    if(size == 0){
        kfree(ptr);
        return nullptr;
    }
    size_t old = usable_size(ptr);
    if(size <= old){
        return ptr;
    }
    void* p = kmalloc(size);
    if(p){
        memcpy(p, ptr, old);
        kfree(ptr);
    }
    return p;
}

void kfree(void* ptr){
    if(!ptr){
        return;
    }
    if(KmemCache* c = slab_cache_of(ptr)){
        kmem_cache_free(c, ptr);
        return;
    }
    if(BigHeader* h = big_header_of(ptr)){
        h->magic = 0;
        free_pages((uint64_t)h - phys_map_base, h->order);
        return;
    }
    panic("kfree: pointer necunoscut %p", ptr);
}
