#pragma once
#include "types.h"

const uint8_t TIMER_VECTOR = 0x30;      // 0x20 + 0x10, dupa IRQ-urile ISA
const uint32_t TIMER_HZ = 1000;

extern volatile uint64_t jiffies;

// Calibreaza TSC si LAPIC timer (referinta HPET sau PIT) si porneste tick-ul de 1000 Hz
void clock_init();
uint64_t monotonic_ns();
uint64_t uptime_ms();
// Blocheaza task-ul curent (scheduler); in task 0 asteapta cu hlt. Cere intreruperile active
void sleep_ms(uint32_t ms);
