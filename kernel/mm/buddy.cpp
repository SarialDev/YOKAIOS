#include "mm/buddy.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

// Pana la HHDM (T3.3) doar primul GiB e mapat (la KERNEL_VMA), deci doar el intra in buddy
const uint64_t ACCESS_LIMIT = 1UL << 30;
const uint64_t LIMIT_4G = 1UL << 32;
const uint32_t FREE_MAGIC = 0xB0DD1E5F;

// Antet scris la inceputul fiecarui bloc liber
struct FreeBlock {
    FreeBlock* next;
    FreeBlock* prev;
    uint32_t magic;
    uint32_t order;
};

FreeBlock* free_lists[BUDDY_MAX_ORDER + 1];
uint64_t managed_pages = 0;
uint64_t free_count = 0;

// Lock simplu pe un singur CPU: dezactiveaza intreruperile si le reactiveaza doar daca erau active
class IrqLock {
public:
    IrqLock(){
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
        was_enabled = rflags & (1 << 9);
    }
    ~IrqLock(){
        if(was_enabled){
            __asm__ volatile("sti" : : : "memory");
        }
    }
private:
    bool was_enabled;
};

uint64_t block_bytes(int order){
    return PAGE_SIZE << order;
}

FreeBlock* to_block(uint64_t phys){
    return (FreeBlock*)(KERNEL_VMA + phys);
}

uint64_t to_phys(const FreeBlock* b){
    return (uint64_t)b - KERNEL_VMA;
}

void push(uint64_t phys, int order){
    FreeBlock* b = to_block(phys);
    b->magic = FREE_MAGIC;
    b->order = order;
    b->prev = nullptr;
    b->next = free_lists[order];
    if(b->next){
        b->next->prev = b;
    }
    free_lists[order] = b;
}

void remove(FreeBlock* b){
    if(b->prev){
        b->prev->next = b->next;
    } else {
        free_lists[b->order] = b->next;
    }
    if(b->next){
        b->next->prev = b->prev;
    }
    b->magic = 0;
}

bool range_free(uint64_t page, uint64_t count){
    for(uint64_t i = 0; i < count; i++){
        if(!pmm_page_free(page + i)){
            return false;
        }
    }
    return true;
}

}

void buddy_init(){
    uint64_t total = pmm_total_pages();
    managed_pages = total < ACCESS_LIMIT / PAGE_SIZE ? total : ACCESS_LIMIT / PAGE_SIZE;

    // Cel mai mare bloc aliniat si complet liber care incepe la fiecare pagina libera
    for(uint64_t p = 0; p < managed_pages; ){
        if(!pmm_page_free(p)){
            p++;
            continue;
        }
        int order = 0;
        while(order < BUDDY_MAX_ORDER){
            uint64_t next = 1UL << (order + 1);
            if(p % next || p + next > managed_pages || !range_free(p, next)){
                break;
            }
            order++;
        }
        push(p * PAGE_SIZE, order);
        free_count += 1UL << order;
        p += 1UL << order;
    }

    uint64_t unmanaged = pmm_free_count_bitmap() - free_count;
    kprintf("buddy: ordine 0..%d, %lu MiB peste 1 GiB asteapta HHDM (T3.3)\n", BUDDY_MAX_ORDER,
            unmanaged * PAGE_SIZE >> 20);
    kprintf("[OK] buddy free=%lu MiB\n", free_count * PAGE_SIZE >> 20);
}

uint64_t alloc_pages(int order, uint32_t flags){
    if(order < 0 || order > BUDDY_MAX_ORDER){
        return 0;
    }
    IrqLock lock;
    FreeBlock* found = nullptr;
    int o = order;
    for(; o <= BUDDY_MAX_ORDER && !found; o++){
        for(FreeBlock* b = free_lists[o]; b; b = b->next){
            if(!(flags & ALLOC_BELOW_4G) || to_phys(b) + block_bytes(o) <= LIMIT_4G){
                found = b;
                break;
            }
        }
    }
    if(!found){
        return 0;
    }
    o = found->order;
    uint64_t phys = to_phys(found);
    remove(found);
    // Imparte blocul; jumatatile superioare raman libere
    while(o > order){
        o--;
        push(phys + block_bytes(o), o);
    }
    pmm_mark_used(phys / PAGE_SIZE, 1UL << order);
    free_count -= 1UL << order;
    return phys;
}

void free_pages(uint64_t phys, int order){
    uint64_t page = phys / PAGE_SIZE;
    uint64_t count = 1UL << order;
    if(order < 0 || order > BUDDY_MAX_ORDER || phys % block_bytes(order) || page + count > managed_pages){
        panic("free_pages: bloc invalid %lx ordin %d", phys, order);
    }
    IrqLock lock;
    for(uint64_t i = 0; i < count; i++){
        if(pmm_page_free(page + i)){
            panic("free_pages: double free la %lx", (page + i) * PAGE_SIZE);
        }
    }
    pmm_mark_free(page, count);
    free_count += count;

    // Uneste cu buddy-ul cat timp acesta e un bloc liber intreg de acelasi ordin
    while(order < BUDDY_MAX_ORDER){
        uint64_t buddy = phys ^ block_bytes(order);
        if(buddy / PAGE_SIZE + (1UL << order) > managed_pages || !pmm_page_free(buddy / PAGE_SIZE)){
            break;
        }
        FreeBlock* b = to_block(buddy);
        if(b->magic != FREE_MAGIC || b->order != (uint32_t)order){
            break;
        }
        remove(b);
        phys &= ~block_bytes(order);
        order++;
    }
    push(phys, order);
}

uint64_t buddy_free_count(){
    return free_count;
}
