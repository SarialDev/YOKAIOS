#pragma once
#include "types.h"
#include "mm/vmm.h"

// brk / mmap anonim / munmap (mem.cpp), legate in table.cpp
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off);
int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t);
// Elibereaza paginile user mapate din [from, to) (cele lipsa se sar)
void unmap_range(AddressSpace& as, uint64_t from, uint64_t to);
