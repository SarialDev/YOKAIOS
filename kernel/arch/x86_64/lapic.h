#pragma once
#include "types.h"

const uint8_t LAPIC_SPURIOUS_VECTOR = 0xFF;

// Mapeaza LAPIC-ul de la adresa din MADT si il porneste prin SVR
void lapic_init(uint64_t phys);
bool lapic_enabled();
void lapic_eoi();
uint32_t lapic_id();

// Numara tick-urile timerului LAPIC (divizor 16) cat dureaza wait_ms(ms)
uint32_t lapic_timer_measure(void (*wait_ms)(uint32_t), uint32_t ms);
// Mod periodic: intrerupere pe vector la fiecare `ticks` tick-uri (divizor 16)
void lapic_timer_periodic(uint32_t ticks, uint8_t vector);
