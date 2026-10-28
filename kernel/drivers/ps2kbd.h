#pragma once
#include "types.h"

const uint8_t KBD_IRQ = 1;

// Goleste controlerul, inregistreaza tastatura in HAL si porneste IRQ1; scancode-urile ajung pe event bus (hal/input.h)
void ps2kbd_init();
