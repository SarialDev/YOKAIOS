#pragma once
#include "types.h"

static inline uint64_t rdtsc(){
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// CPUID 0x80000007 EDX bit 8: TSC cu frecventa constanta
bool tsc_invariant();
// CPUID 1 ECX bit 31: rulam intr-o masina virtuala (TSC-ul virtual are rata constanta)
bool cpu_hypervisor();
// Masoara frecventa TSC pe ms milisecunde cu functia de asteptare data; rezultatul in Hz
uint64_t tsc_calibrate(void (*wait_ms)(uint32_t), uint32_t ms);
