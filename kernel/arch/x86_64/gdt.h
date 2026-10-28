#pragma once
#include "types.h"

struct PerCpu;

const uint16_t GDT_KCODE = 0x08;
const uint16_t GDT_KDATA = 0x10;
const uint16_t GDT_UDATA = 0x18 | 3;
const uint16_t GDT_UCODE = 0x20 | 3;
const uint16_t GDT_TSS   = 0x28;

// GDT si TSS proprii CPU-ului c (in PerCpu), incarcate pe CPU-ul curent; c->ist1 trebuie alocat
void gdt_init_cpu(PerCpu& c);
// TSS.rsp0 al CPU-ului curent
void tss_set_rsp0(uint64_t rsp0);
