#pragma once
#include "interrupt.h"

const uint8_t IDT_VEC_NMI = 2;
const uint8_t IDT_VEC_DF = 8;
const uint8_t IDT_VEC_SYSCALL = 0x80;

void idt_init();
