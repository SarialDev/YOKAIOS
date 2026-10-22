#pragma once
#include "types.h"

const uint64_t PAGE_SIZE = 4096;

// Bitmap 1 bit / pagina 4 KiB (1 = ocupata) peste RAM-ul din harta Multiboot2
void pmm_init();
// Adresa fizica a unei pagini libere, 0 daca nu mai e memorie
uint64_t pmm_alloc_page();
// Elibereaza o pagina; panic la double free sau adresa invalida
void pmm_free_page(uint64_t phys);
// n pagini consecutive; adresa primei pagini sau 0
uint64_t pmm_alloc_contig(uint64_t n);
uint64_t pmm_free_count();
