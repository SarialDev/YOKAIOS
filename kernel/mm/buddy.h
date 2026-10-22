#pragma once
#include "types.h"

const int BUDDY_MAX_ORDER = 10;                 // 2^10 pagini = 4 MiB
const uint32_t ALLOC_BELOW_4G = 1 << 0;         // pentru dispozitive DMA pe 32 de biti

// Construieste listele libere din bitmap-ul pmm (apelat din pmm_init)
void buddy_init();
// Reconstruieste listele dupa ce se schimba maparea memoriei fizice (vmm_init: HHDM)
void buddy_rebuild();
// Bloc de 2^order pagini aliniat la marimea lui; 0 daca nu exista
uint64_t alloc_pages(int order, uint32_t flags = 0);
// Panic la double free, adresa nealiniata sau in afara memoriei gestionate
void free_pages(uint64_t phys, int order);
uint64_t buddy_free_count();
