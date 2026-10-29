#pragma once
#include "hal/fb.h"

// Driver de framebuffer liniar 32 bpp (RGB) din tag-ul Multiboot2. Pixelii se scriu atat in framebuffer
// (mapat write-combining prin PAT), cat si in back buffer-ul din RAM, daca exista: scroll-ul citeste
// doar din RAM, fiindca citirea din memorie WC e lenta. Coordonatele in pixeli, originea sus-stanga

// Mapeaza framebuffer-ul din boot_info si il sterge (negru). false daca lipseste sau formatul nu e 32 bpp RGB
bool fb_init();
bool fb_ready();
// Dispozitivul HAL (info()/flush()); nullptr daca fb_init nu a reusit
Framebuffer* fb_device();

uint32_t fb_width();
uint32_t fb_height();
bool fb_write_combining();
bool fb_has_backbuffer();

// Culoarea 0xRRGGBB impachetata in formatul framebuffer-ului
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t fb_color(uint32_t rgb);

void fb_putpixel(int x, int y, uint32_t color);
// Din back buffer daca exista, altfel din framebuffer; 0 in afara ecranului
uint32_t fb_getpixel(int x, int y);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
// Copiaza w x h pixeli deja impachetati (fb_rgb) din src; src_pitch in octeti. Decupat la marginile ecranului
void fb_blit(int x, int y, const uint32_t* src, int w, int h, uint32_t src_pitch);
// Glif monocrom 8 pixeli lat: un octet pe rand, bitul 7 = pixelul din stanga. Intreg in ecran, altfel ignorat
void fb_draw_glyph(int x, int y, const uint8_t* rows, int height, uint32_t fg, uint32_t bg);
// Muta continutul in sus cu dy randuri de pixeli (copiere pe randuri) si umple banda de jos cu fill
void fb_scroll_up(int dy, uint32_t fill);
// Copiaza back buffer-ul in framebuffer (fara efect fara back buffer)
void fb_flush();
