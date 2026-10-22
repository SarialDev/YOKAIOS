#pragma once
#include "types.h"

// Ordinea corespunde push-urilor din isr.S (r15 e ultimul pus, deci primul in memorie)
struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*InterruptHandler)(InterruptFrame*);

void irq_register(uint8_t vec, InterruptHandler h);
