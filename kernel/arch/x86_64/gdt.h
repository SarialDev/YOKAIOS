#pragma once
#include "types.h"

const uint16_t GDT_KCODE = 0x08;
const uint16_t GDT_KDATA = 0x10;
const uint16_t GDT_UDATA = 0x18 | 3;
const uint16_t GDT_UCODE = 0x20 | 3;
const uint16_t GDT_TSS   = 0x28;

void gdt_init();
void tss_set_rsp0(uint64_t rsp0);
