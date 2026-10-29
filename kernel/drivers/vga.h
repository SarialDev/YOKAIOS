#pragma once
#include "types.h"

const int LATIME_ECRAN = 80;
const int INALTIME_ECRAN = 25;

// Curata ecranul si inregistreaza consola VGA text in HAL
void vga_init();
// Opreste consola VGA text (mod grafic activ): nu mai scrie si nu mai e „ecran” pentru HAL
void vga_disable();
