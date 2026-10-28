#pragma once
#include "types.h"
#include "mm/vmm.h"

// Stiva user: 64 KiB sub USER_STACK_TOP
const uint64_t USER_STACK_TOP = 0x00007FFFFFFFE000UL;
const uint64_t USER_STACK_SIZE = 64 * 1024;

// Tipuri din vectorul auxiliar (auxv) pus pe stiva dupa envp
const uint64_t AT_NULL = 0;
const uint64_t AT_PHDR = 3;
const uint64_t AT_PHENT = 4;
const uint64_t AT_PHNUM = 5;
const uint64_t AT_PAGESZ = 6;
const uint64_t AT_ENTRY = 9;

struct ElfImage {
    uint64_t entry;
    uint64_t phdr;              // adresa user a tabelului de program headere, 0 daca nu e incarcat
    uint16_t phent;
    uint16_t phnum;
};

// Verifica un ELF64 static x86-64 si mapeaza segmentele PT_LOAD in as (NX unde nu e X); 0 sau -errno
int64_t elf_load(AddressSpace& as, const uint8_t* data, uint64_t size, ElfImage& img);
// Mapeaza stiva user si pune argc, argv, envp si auxv; rsp-ul initial (aliniat la 16), 0 la eroare
uint64_t elf_setup_stack(AddressSpace& as, const ElfImage& img, const char* const* argv, const char* const* envp);

// Proces nou din modulul Multiboot2 "name": spatiu de adrese, ELF, stiva, task READY. pid-ul sau 0
uint64_t exec_spawn_module(const char* name, const char* const* argv, const char* const* envp);
