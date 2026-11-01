#include "syscall/mem.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "proc/process.h"
#include "exec/elf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "lib/string.h"

namespace {

const uint64_t BRK_MAX = 1UL << 30;             // heap-ul brk: cel mult 1 GiB peste sfarsitul imaginii

uint64_t page_up(uint64_t v){
    return (v + PAGE_4K - 1) & ~(PAGE_4K - 1);
}

// Pagini zero RW+NX pe [from, to); la lipsa de memorie le elibereaza pe cele deja mapate si da false
bool map_range(AddressSpace& as, uint64_t from, uint64_t to, uint64_t flags){
    for(uint64_t v = from; v < to; v += PAGE_4K){
        uint64_t phys = pmm_alloc_page();
        if(!phys){
            unmap_range(as, from, v);
            return false;
        }
        memset(phys_to_virt(phys), 0, PAGE_4K);
        if(!vmm_map(as, v, phys, flags | PTE_USER)){
            pmm_free_page(phys);
            unmap_range(as, from, v);
            return false;
        }
    }
    return true;
}

}

void unmap_range(AddressSpace& as, uint64_t from, uint64_t to){
    for(uint64_t v = from; v < to; v += PAGE_4K){
        uint64_t e = vmm_entry(as, v);
        if(e && (e & PTE_USER)){
            vmm_unmap(as, v);
            pmm_free_page(e & PTE_ADDR_MASK);
        }
    }
}

// brk(0) si orice cerere invalida intorc break-ul curent (ca pe Linux)
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    if(addr < p->brk_base || addr - p->brk_base > BRK_MAX){
        return (int64_t)p->brk_cur;
    }
    AddressSpace& as = *task_current()->as;
    uint64_t old_top = page_up(p->brk_cur);
    uint64_t new_top = page_up(addr);
    if(new_top > old_top){
        if(!map_range(as, old_top, new_top, PTE_RW | PTE_NX)){
            return (int64_t)p->brk_cur;
        }
    } else if(new_top < old_top){
        unmap_range(as, new_top, old_top);
    }
    p->brk_cur = addr;
    return (int64_t)addr;
}

// Doar anonim si privat; adresa o alege kernelul (hint-ul si MAP_FIXED nu se folosesc)
int64_t sys_mmap(uint64_t, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t){
    Process* p = process_current();
    if(!p || len == 0 || len > MMAP_END - MMAP_BASE){
        return -EINVAL;
    }
    if(!(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE) || (int32_t)fd != -1){
        return -ENODEV;
    }
    len = page_up(len);
    if(p->mmap_next + len > MMAP_END){
        return -ENOMEM;
    }
    AddressSpace& as = *task_current()->as;
    uint64_t pf = (prot & PROT_WRITE ? PTE_RW : 0) | (prot & PROT_EXEC ? 0 : PTE_NX);
    // Paginile mapate sunt mereu citibile (fara PROT_NONE)
    if(!map_range(as, p->mmap_next, p->mmap_next + len, pf)){
        return -ENOMEM;
    }
    uint64_t addr = p->mmap_next;
    p->mmap_next += len;
    return (int64_t)addr;
}

int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p || (addr & (PAGE_4K - 1)) || len == 0){
        return -EINVAL;
    }
    len = page_up(len);
    if(addr < MMAP_BASE || addr + len < addr || addr + len > p->mmap_next){
        return -EINVAL;
    }
    unmap_range(*task_current()->as, addr, addr + len);
    return 0;
}
