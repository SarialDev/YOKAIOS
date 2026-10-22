#include "mm/slab.h"
#include "mm/buddy.h"
#include "mm/pmm.h"
#include "arch/x86_64/paging.h"
#include "core/panic.h"
#include "irqlock.h"

// Header la inceputul fiecarei pagini de slab
struct SlabHeader {
    uint32_t magic;
    uint32_t inuse;
    KmemCache* cache;
    SlabHeader* next;
    SlabHeader* prev;
    void* freelist;             // primul cuvant din fiecare obiect liber = urmatorul liber
};

struct KmemCache {
    const char* name;
    size_t obj_size;
    size_t first_offset;        // primul obiect, dupa header si aliniat
    uint32_t objs_per_slab;
    SlabHeader* partial;        // slab-uri cu cel putin un obiect liber
    SlabHeader* full;
};

namespace {

const uint32_t SLAB_MAGIC = 0x51AB51AB;

// Cache-ul pentru structurile KmemCache insesi
KmemCache cache_cache;
bool cache_cache_ready = false;

size_t align_up(size_t v, size_t a){
    return (v + a - 1) & ~(a - 1);
}

void list_add(SlabHeader*& head, SlabHeader* s){
    s->prev = nullptr;
    s->next = head;
    if(head){
        head->prev = s;
    }
    head = s;
}

void list_remove(SlabHeader*& head, SlabHeader* s){
    if(s->prev){
        s->prev->next = s->next;
    } else {
        head = s->next;
    }
    if(s->next){
        s->next->prev = s->prev;
    }
}

void init_cache(KmemCache* c, const char* name, size_t size, size_t align){
    if(align < 8){
        align = 8;
    }
    c->name = name;
    c->obj_size = align_up(size < 8 ? 8 : size, align);
    c->first_offset = align_up(sizeof(SlabHeader), align);
    c->objs_per_slab = (PAGE_SIZE - c->first_offset) / c->obj_size;
    c->partial = nullptr;
    c->full = nullptr;
    if(c->objs_per_slab == 0){
        panic("kmem_cache_create(%s): obiect de %lu bytes nu incape intr-un slab", name, size);
    }
}

SlabHeader* new_slab(KmemCache* c){
    uint64_t phys = alloc_pages(0);
    if(!phys){
        return nullptr;
    }
    SlabHeader* s = (SlabHeader*)phys_to_virt(phys);
    s->magic = SLAB_MAGIC;
    s->inuse = 0;
    s->cache = c;
    s->freelist = nullptr;
    uint8_t* base = (uint8_t*)s + c->first_offset;
    for(int i = c->objs_per_slab - 1; i >= 0; i--){
        void* obj = base + i * c->obj_size;
        *(void**)obj = s->freelist;
        s->freelist = obj;
    }
    return s;
}

SlabHeader* header_of(const void* obj){
    return (SlabHeader*)((uint64_t)obj & ~(PAGE_SIZE - 1));
}

}

KmemCache* kmem_cache_create(const char* name, size_t size, size_t align){
    if(!cache_cache_ready){
        init_cache(&cache_cache, "kmem_cache", sizeof(KmemCache), alignof(KmemCache));
        cache_cache_ready = true;
    }
    KmemCache* c = (KmemCache*)kmem_cache_alloc(&cache_cache);
    if(c){
        init_cache(c, name, size, align);
    }
    return c;
}

void* kmem_cache_alloc(KmemCache* c){
    IrqLock lock;
    SlabHeader* s = c->partial;
    if(!s){
        s = new_slab(c);
        if(!s){
            return nullptr;
        }
        list_add(c->partial, s);
    }
    void* obj = s->freelist;
    s->freelist = *(void**)obj;
    s->inuse++;
    if(!s->freelist){
        list_remove(c->partial, s);
        list_add(c->full, s);
    }
    return obj;
}

void kmem_cache_free(KmemCache* c, void* obj){
    IrqLock lock;
    SlabHeader* s = header_of(obj);
    uint64_t offset = (uint64_t)obj - (uint64_t)s;
    if(s->magic != SLAB_MAGIC || s->cache != c || offset < c->first_offset ||
       (offset - c->first_offset) % c->obj_size || (offset - c->first_offset) / c->obj_size >= c->objs_per_slab){
        panic("kmem_cache_free(%s): pointer invalid %p", c->name, obj);
    }
    for(void* f = s->freelist; f; f = *(void**)f){
        if(f == obj){
            panic("kmem_cache_free(%s): double free la %p", c->name, obj);
        }
    }
    bool was_full = !s->freelist;
    *(void**)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    if(was_full){
        list_remove(c->full, s);
        list_add(c->partial, s);
    }
    // Slab-ul gol se intoarce in buddy, ca memoria folosita sa nu creasca
    if(s->inuse == 0){
        list_remove(c->partial, s);
        s->magic = 0;
        free_pages((uint64_t)s - phys_map_base, 0);
    }
}

size_t kmem_cache_object_size(const KmemCache* c){
    return c->obj_size;
}

KmemCache* slab_cache_of(const void* obj){
    const SlabHeader* s = header_of(obj);
    return s->magic == SLAB_MAGIC ? s->cache : nullptr;
}
