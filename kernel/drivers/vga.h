#pragma once
#include "types.h"

const int LATIME_ECRAN = 80;
const int INALTIME_ECRAN = 25;

void curata_ecran();
void out(const char* text, int x, int y);
void muta_cursor(int x, int y);

// Consola de log: cursor propriu, \n si scroll
void vga_putc(char c);
