#pragma once
#include "types.h"
#include "fs/vfs/inode.h"

// FAT32 (fara exFAT): BPB + FSInfo, cache de FAT, lanturi de clustere, directoare cu nume lungi (LFN) citite si
// scrise, creare / stergere / redenumire de fisiere si directoare, extinderea fisierelor. Tip de sistem de fisiere
// "fat32"; sursa montarii = numele unui dispozitiv bloc din HAL ("sda1"), optiunea "ro" monteaza read-only.
// Fisierele merg prin cache-ul de blocuri (block/bcache.h) al dispozitivului: un volum se monteaza pe partitie
// (sda1) sau pe discul intreg (sda), niciodata pe amandoua.
void fat32_init();

// Monteaza pe /media prima partitie (sau primul disc) cu BPB FAT32 din HAL. 0, -ENODEV daca nu exista niciuna,
// altfel -errno al montarii
int fat32_automount();

// Numarul de clustere libere ale volumului lui ino (pentru teste)
uint32_t fat32_free_clusters(const Inode* ino);
