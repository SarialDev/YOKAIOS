#pragma once
#include "types.h"
#include "mm/vmm.h"

// Stiva user: ultimii 64 KiB sub USER_STACK_TOP se mapeaza la exec; restul pana la USER_STACK_MAX vine la cerere (page fault).
// Pagina de la USER_STACK_TOP este trambulina de semnale (proc/signal.h)
const uint64_t USER_STACK_TOP = 0x00007FFFFFFFE000UL;
const uint64_t USER_STACK_SIZE = 64 * 1024;
const uint64_t USER_STACK_MAX = 8UL << 20;

// mmap: adresele fara hint se aleg prima gaura libera de aici pana la MMAP_END (mm/vma.cpp)
const uint64_t MMAP_BASE = 0x0000400000000000UL;
const uint64_t MMAP_END = 0x0000700000000000UL;

// Tipuri din vectorul auxiliar (auxv) pus pe stiva dupa envp
const uint64_t AT_NULL = 0;
const uint64_t AT_PHDR = 3;
const uint64_t AT_PHENT = 4;
const uint64_t AT_PHNUM = 5;
const uint64_t AT_PAGESZ = 6;
const uint64_t AT_ENTRY = 9;

const int ELF_MAX_SEGS = 16;

struct ElfSeg {
    uint64_t start, end;        // aliniate la pagina
    uint32_t prot;              // PROT_*
};

struct ElfImage {
    uint64_t entry;
    uint64_t phdr;              // adresa user a tabelului de program headere, 0 daca nu e incarcat
    uint16_t phent;
    uint16_t phnum;
    int nseg;
    ElfSeg seg[ELF_MAX_SEGS];   // segmentele PT_LOAD, pentru VMA-urile procesului
    uint64_t end;               // primul octet dupa ultimul segment (rotunjit la pagina): de aici incepe brk
};

// Verifica un ELF64 static x86-64 si mapeaza segmentele PT_LOAD in as (NX unde nu e X); 0 sau -errno
int64_t elf_load(AddressSpace& as, const uint8_t* data, uint64_t size, ElfImage& img);
// Mapeaza stiva user si pune argc, argv, envp si auxv; rsp-ul initial (aliniat la 16), 0 la eroare
uint64_t elf_setup_stack(AddressSpace& as, const ElfImage& img, const char* const* argv, const char* const* envp);

// Proces nou (fara parinte, fd 0-2 = consola, cwd = radacina) din fisierul ELF de la path (citit prin VFS): spatiu de
// adrese, ELF, stiva, task READY. pid-ul sau 0
uint64_t exec_spawn(const char* path, const char* const* argv, const char* const* envp);
// execve: inlocuieste imaginea procesului curent cu fisierul ELF de la path (relativ la cwd; fd-urile raman) si sare
// in ring 3. argv/envp trebuie sa fie in memoria kernelului; release (kmalloc, poate fi nullptr) se elibereaza inainte
// de salt. Revine doar la eroare, cu -errno (-ENOENT, -ENOEXEC, ...), cu imaginea veche intacta (si release neeliberat)
int64_t exec_replace(const char* path, const char* const* argv, const char* const* envp, void* release);
