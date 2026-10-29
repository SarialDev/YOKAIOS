#pragma once
#include "types.h"

// PAT: intrarea 1 (doar PTE_PWT) devine write-combining. Se apeleaza pe fiecare CPU inainte sa
// atinga o pagina WC; fara PAT in CPU intoarce false si paginile raman necache-uite
bool pat_init_cpu();
// true daca CPU-ul (BSP) are PAT
bool pat_supported();
