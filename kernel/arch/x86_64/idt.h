#pragma once
#include "interrupt.h"

const uint8_t IDT_VEC_NMI = 2;
const uint8_t IDT_VEC_DF = 8;
const uint8_t IDT_VEC_SYSCALL = 0x80;
// Vectori de intreruperi hardware (ISA 0x20-0x2F, LAPIC 0x30-0x3F): primesc EOI
const uint8_t IDT_VEC_IRQ_FIRST = 0x20;
const uint8_t IDT_VEC_IRQ_END = 0x40;

void idt_init();
// Incarca IDT-ul comun pe CPU-ul curent (AP)
void idt_load();
