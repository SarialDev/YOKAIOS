#pragma once
#include "types.h"

// Cauta tabelul ACPI "HPET", mapeaza registrele si porneste contorul principal
bool hpet_init();
bool hpet_present();
void hpet_wait_ms(uint32_t ms);
