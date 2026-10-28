#pragma once
#include "types.h"

// Argumentele vin in ordinea ABI: rdi rsi rdx r10 r8 r9
typedef int64_t (*SyscallFn)(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

// Handlerul pentru numarul nr, nullptr daca nu exista (-ENOSYS)
SyscallFn syscall_lookup(uint64_t nr);
