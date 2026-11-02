#pragma once
#include "types.h"

// KASLR (boot/kaslr.c): imaginea kernelului ruleaza la KERNEL_VMA + kaslr_offset (multiplu de 2 MiB, ales la boot),
// fizic tot de la 1 MiB. Adresele fizice ale simbolurilor din imagine se calculeaza doar prin kernel_phys
extern "C" uint64_t kaslr_offset;
extern "C" uint32_t kaslr_flags;            // bit 0: offset din RDRAND, bit 1: cmdline "nokaslr"

// Barierea asm opreste compilatorul sa plieze "simbol - KERNEL_VMA" intr-o relocare R_X86_64_32 (valoare fizica de la
// link, pe care KASLR nu o corecteaza): adresa se ia intai intreaga (relocata), apoi se scade baza de la rulare
inline uint64_t kernel_phys(const void* p){
    uint64_t v = (uint64_t)p;
    __asm__("" : "+r"(v));
    return v - KERNEL_VMA - kaslr_offset;
}

inline const char* kaslr_source(){
    return kaslr_flags & 2 ? "nokaslr" : kaslr_flags & 1 ? "RDRAND" : "TSC+RTC";
}
