#pragma once
#include "types.h"

// Mapeaza IOAPIC-urile din MADT si mascheaza toate intrarile
void ioapic_init();
// IRQ ISA -> vector pe LAPIC-ul dat; aplica Interrupt Source Overrides. false daca nu exista IOAPIC pentru GSI
bool ioapic_route(uint8_t irq, uint8_t vector, uint8_t lapic_id);
