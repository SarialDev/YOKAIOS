#pragma once
#include "types.h"

// brk / mmap / munmap / mprotect / msync (mem.cpp -> mm/vma.cpp), legate in table.cpp
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off);
int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, uint64_t, uint64_t, uint64_t);
int64_t sys_msync(uint64_t addr, uint64_t len, uint64_t flags, uint64_t, uint64_t, uint64_t);
