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

// ASLR (cmdline "norandmaps" il opreste): la fiecare exec se aleg, din entropie RDRAND (fallback TSC), varful stivei,
// baza mmap, golul dintre imagine si brk si baza unui ELF ET_DYN (static-pie). Granularitate: pagina de 4 KiB
const uint64_t ET_DYN_BASE = 0x0000555500000000UL;     // PIE: ET_DYN_BASE + [0, 2^28) pagini (1 TiB)
const uint64_t ASLR_PIE_PAGES = 1UL << 28;
const uint64_t ASLR_MMAP_PAGES = 1UL << 28;             // mmap_base = MMAP_BASE + [0, 1 TiB)
const uint64_t ASLR_STACK_PAGES = 1UL << 22;            // stack_top = USER_STACK_TOP - [0, 16 GiB)
const uint64_t ASLR_BRK_PAGES = 1UL << 13;              // brk_base = sfarsitul imaginii + [0, 32 MiB)

struct UserLayout {
    uint64_t pie_base;          // unde se incarca primul segment al unui ET_DYN
    uint64_t stack_top;
    uint64_t sp_offset;         // primul rsp e cu atat sub stack_top (0..4080, multiplu de 16)
    uint64_t mmap_base;
    uint64_t brk_gap;
};

bool aslr_enabled();
// Asezarea unui proces nou; fara ASLR: valorile fixe de dinainte (ET_DYN_BASE, USER_STACK_TOP, MMAP_BASE, 0)
UserLayout aslr_layout();

// Tipuri din vectorul auxiliar (auxv) pus pe stiva dupa envp
const uint64_t AT_NULL = 0;
const uint64_t AT_PHDR = 3;
const uint64_t AT_PHENT = 4;
const uint64_t AT_PHNUM = 5;
const uint64_t AT_PAGESZ = 6;
const uint64_t AT_BASE = 7;
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
    uint64_t bias;              // ET_DYN: cat s-a adunat la adresele din fisier (0 la ET_EXEC)
    uint64_t nrelocs;           // ET_DYN: relocari R_X86_64_RELATIVE aplicate
};

// Verifica un ELF64 static x86-64 (ET_EXEC, sau ET_DYN static-pie incarcat la pie_base cu relocarile
// R_X86_64_RELATIVE aplicate) si mapeaza segmentele PT_LOAD in as (NX unde nu e X); 0 sau -errno
int64_t elf_load(AddressSpace& as, const uint8_t* data, uint64_t size, ElfImage& img, uint64_t pie_base = ET_DYN_BASE);
// Mapeaza stiva user sub stack_top si pune argc, argv, envp si auxv (cu sp_offset octeti mai jos); rsp-ul initial
// (aliniat la 16), 0 la eroare
uint64_t elf_setup_stack(AddressSpace& as, const ElfImage& img, const char* const* argv, const char* const* envp,
                         uint64_t stack_top = USER_STACK_TOP, uint64_t sp_offset = 0);

// Proces nou (fara parinte, fd 0-2 = consola, cwd = radacina) din fisierul ELF de la path (citit prin VFS): spatiu de
// adrese, ELF, stiva, task READY. pid-ul sau 0
uint64_t exec_spawn(const char* path, const char* const* argv, const char* const* envp);
// execve: inlocuieste imaginea procesului curent cu fisierul ELF de la path (relativ la cwd; fd-urile raman) si sare
// in ring 3. argv/envp trebuie sa fie in memoria kernelului; release (kmalloc, poate fi nullptr) se elibereaza inainte
// de salt. Revine doar la eroare, cu -errno (-ENOENT, -ENOEXEC, ...), cu imaginea veche intacta (si release neeliberat)
int64_t exec_replace(const char* path, const char* const* argv, const char* const* envp, void* release);
