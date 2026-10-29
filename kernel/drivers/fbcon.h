#pragma once
#include "types.h"

// Consola text peste framebuffer (128x48 la 1024x768, glife 8x16, tema Dark), inregistrata in HAL.
// Intoarce false (consola VGA text ramane) fara framebuffer, cu cmdline "nofb" sau daca maparea esueaza
bool fbcon_init();
bool fbcon_active();

// Pentru teste: celula = caracter | (atribut VGA << 8); pozitia curenta de scriere (putc)
uint16_t fbcon_cell(int col, int row);
void fbcon_log_pos(int& col, int& row);
// Culoarea impachetata a intrarii din paleta de 16 culori (index VGA)
uint32_t fbcon_palette(int idx);
