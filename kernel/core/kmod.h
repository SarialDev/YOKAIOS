#pragma once
#include "types.h"

// Module de kernel incarcabile (T8.5): ELF relocabil (ET_REL x86-64) urmat de semnatura Ed25519 (core/verify.h).
// Semnatura se verifica INAINTE de orice parsare a ELF-ului; un modul nesemnat sau modificat cu un octet nu se incarca.
//
// Formatul modulului (C, compilat cu -fPIC -fno-plt -mno-red-zone -mno-sse -fno-stack-protector -ffreestanding -c):
//   const char yokai_module_name[] = "nume";      // obligatoriu, [a-z0-9_]{1,31}
//   int  yokai_module_init(void);                 // obligatoriu; 0 = ok
//   void yokai_module_exit(void);                 // optional
// Simbolurile nedefinite se rezolva doar din tabela de simboluri exportate de kernel (kmod.cpp: kprintf, kmalloc, ...).
// Relocari suportate: R_X86_64_64, PC32, PLT32 (cu stub), GOTPCREL / GOTPCRELX / REX_GOTPCRELX (cu intrare GOT proprie).

const uint64_t KMOD_MAX_IMAGE = 256 * 1024;
const int KMOD_MAX = 8;

// Verifica semnatura, incarca, relocheaza, mapeaza (text RX, rodata R, date RW) si apeleaza yokai_module_init.
// 0 sau -errno (-ENOKEY nesemnat, -EKEYREJECTED semnatura gresita, -ENOEXEC ELF invalid / relocare nesuportata,
// -ENOENT simbol nerezolvat, -EEXIST nume deja incarcat, -ENOMEM, -E2BIG)
int kmod_load(const void* blob, uint64_t len);
// Apeleaza yokai_module_exit si elibereaza modulul; -ENOENT daca nu e incarcat
int kmod_unload(const char* name);
bool kmod_loaded(const char* name);
int kmod_count();

int64_t sys_init_module(uint64_t image, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_delete_module(uint64_t name, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
