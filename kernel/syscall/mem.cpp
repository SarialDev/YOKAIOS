#include "syscall/mem.h"
#include "uapi/syscall_nr.h"
#include "proc/process.h"
#include "mm/vma.h"

// brk(0) si orice cerere invalida intorc break-ul curent (ca pe Linux). Heap-ul e o zona VMA_HEAP, paginile vin la cerere
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    if(!process_current()){
        return -EINVAL;
    }
    return (int64_t)vma_brk(addr);
}

// Anonim sau pe fisier, MAP_PRIVATE (CoW) sau MAP_SHARED, hint / MAP_FIXED (mm/vma.cpp)
int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off){
    return vma_mmap(addr, len, prot, flags, (int64_t)(int32_t)fd, off);
}

int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t){
    return vma_munmap(addr, len);
}

int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, uint64_t, uint64_t, uint64_t){
    return vma_mprotect(addr, len, prot);
}

int64_t sys_msync(uint64_t addr, uint64_t len, uint64_t flags, uint64_t, uint64_t, uint64_t){
    return vma_msync(addr, len, flags);
}
