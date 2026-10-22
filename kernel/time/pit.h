#pragma once
#include "types.h"

// Asteptare activa pe PIT canalul 2 (fara IRQ); referinta de calibrare cand lipseste HPET
void pit_wait_ms(uint32_t ms);
