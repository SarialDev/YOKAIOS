#pragma once
#include "types.h"

const uint8_t LAPIC_SPURIOUS_VECTOR = 0xFF;

// Mapeaza LAPIC-ul de la adresa din MADT si il porneste prin SVR
void lapic_init(uint64_t phys);
bool lapic_enabled();
void lapic_eoi();
uint32_t lapic_id();
