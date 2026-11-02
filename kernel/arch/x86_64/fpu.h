#pragma once
#include "types.h"

// Stare x87/SSE (FXSAVE): kernelul e compilat fara SSE, doar userland-ul o foloseste.
// Salvata/restaurata la fiecare comutare de task (sched.cpp), copiata la fork, resetata la execve.
const size_t FPU_STATE_SIZE = 512;

// BSP: activeaza SSE (CR0.EM=0, CR0.MP=1, CR4.OSFXSR/OSXMMEXCPT), fninit + MXCSR implicit si retine starea implicita
void fpu_init();
// AP-ul are deja CR0/CR4 copiate de la BSP: doar starea implicita
void fpu_init_cpu();

// area: FPU_STATE_SIZE bytes, aliniat la 16
void fpu_save(void* area);
void fpu_restore(const void* area);
// Starea implicita (dupa fninit, MXCSR = 0x1F80), pentru task-uri noi si execve
const void* fpu_default_state();
// Pune starea implicita pe CPU-ul curent (execve)
void fpu_reset();
