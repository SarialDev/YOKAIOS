#pragma once
#include "types.h"

const uint64_t PAGE_SIZE = 4096;

// Bitmap 1 bit / pagina 4 KiB (1 = ocupata) peste RAM-ul din harta Multiboot2; porneste si buddy
void pmm_init();
// Adresa fizica a unei pagini libere, 0 daca nu mai e memorie (alloc_pages(0))
uint64_t pmm_alloc_page();
// Elibereaza o pagina; panic la double free sau adresa invalida
void pmm_free_page(uint64_t phys);
// n pagini consecutive; adresa primei pagini sau 0
uint64_t pmm_alloc_contig(uint64_t n);
// Pagini libere disponibile prin buddy
uint64_t pmm_free_count();

// Folosite de buddy: bitmap-ul ramane sursa de adevar pentru starea paginilor
uint64_t pmm_total_pages();
uint64_t pmm_free_count_bitmap();
bool pmm_page_free(uint64_t page);
void pmm_mark_used(uint64_t page, uint64_t count);
void pmm_mark_free(uint64_t page, uint64_t count);
