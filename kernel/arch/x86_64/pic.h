#pragma once
#include "types.h"

const uint8_t PIC_IRQ_BASE = 0x20;     // IRQ0-7 -> 0x20-0x27, IRQ8-15 -> 0x28-0x2F
const uint8_t PIC_IRQ_COUNT = 16;

// Remapeaza ambele PIC-uri si mascheaza toate liniile
void pic_init();
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_eoi(uint8_t irq);
