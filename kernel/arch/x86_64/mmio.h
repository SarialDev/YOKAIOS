#pragma once
#include "types.h"

// Fereastra MMIO: ultimul GiB virtual (0xFFFFFFFFC0000000), pagini de 2 MiB fara cache.
// Temporar pana la VMM (T3); mapari permanente, fara unmap.
void* mmio_map(uint64_t phys, uint64_t size);

const uint64_t MMIO_WINDOW_BASE = 0xFFFFFFFFC0000000UL;
// PD-ul ferestrei, ca vmm sa-l puna in tabelele noi
uint64_t mmio_pd_phys();
