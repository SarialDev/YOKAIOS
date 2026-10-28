#pragma once
#include "types.h"

const uint8_t TIMER_VECTOR = 0x30;      // 0x20 + 0x10, dupa IRQ-urile ISA
const uint32_t TIMER_HZ = 1000;

extern volatile uint64_t jiffies;

// Calibreaza TSC si LAPIC timer (referinta HPET sau PIT) si porneste tick-ul de 1000 Hz
void clock_init();
// Porneste tick-ul LAPIC pe un AP (cu calibrarea BSP-ului)
void clock_init_ap();
uint64_t monotonic_ns();
// Reprogrameaza timer-ul one-shot dupa ce s-a schimbat cel mai apropiat eveniment sau dupa ce CPU-ul
// a iesit din idle (tick-ul porneste din nou); intreruperile oprite
void clock_reprogram();
// true daca timer-ul merge one-shot (evenimente RT la rezolutia LAPIC), false daca e periodic la 1 ms
bool clock_high_res();
uint64_t uptime_ms();
// Blocheaza task-ul curent (scheduler); in task 0 asteapta cu hlt. Cere intreruperile active
void sleep_ms(uint32_t ms);
