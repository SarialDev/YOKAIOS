#pragma once
#include "types.h"
#include "proc/file.h"

// Memorie partajata POSIX (T10.2): obiecte cu nume ("/nume"), create cu shm_open, dimensionate cu ftruncate si
// mapate cu mmap(MAP_SHARED) de procese neinrudite (buferele ferestrelor compozitorului). Un obiect are paginile lui
// (zero la creare); fiecare mapare ia o referinta pe pagina (pageref), deci paginile traiesc cat cea mai lunga mapare.
// Obiectul dispare cand e scos din nume (shm_unlink) si nu mai e deschis de nimeni (maparile tin File-ul deschis).

const uint32_t SHM_NAME_MAX = 64;           // cu tot cu '/' initial si terminator
const uint64_t SHM_SIZE_MAX = 64UL << 20;   // 64 MiB per obiect

bool shm_is(const File* f);
uint64_t shm_size(const File* f);
// Adresa fizica a paginii de la off (aliniat la pagina) din obiect; -ENXIO in afara obiectului
int64_t shm_page(File* f, uint64_t off);
// ftruncate pe un fd de shm: marire (pagini zero) sau micsorare
int shm_truncate(File* f, uint64_t size);
// Cate obiecte exista (pentru teste: nu se pierd)
uint64_t shm_count();

int64_t sys_shm_open(uint64_t name, uint64_t flags, uint64_t mode, uint64_t, uint64_t, uint64_t);
int64_t sys_shm_unlink(uint64_t name, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_ftruncate(uint64_t fd, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t);
