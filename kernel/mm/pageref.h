#pragma once
#include "types.h"

// Numarul de referinte la paginile fizice partajate (fork cu CoW, MAP_SHARED). Se retin doar referintele in plus:
// 0 = un singur proprietar. Paginile user se elibereaza mereu prin page_put, nu direct prin pmm_free_page
void pageref_init();
// O referinta in plus (pagina devine partajata)
void page_ref(uint64_t phys);
// Elibereaza o referinta; la ultima pagina revine in buddy
void page_put(uint64_t phys);
// Mai are pagina si alti proprietari?
bool page_shared(uint64_t phys);
