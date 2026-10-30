#pragma once
#include "types.h"

// Sistem de fisiere read-only peste o arhiva ustar din memorie (initrd). Se monteaza cu
// vfs_mount(base, "initrd", "/", "tarfs", 0, nullptr): dev_name = numele unui modul Multiboot2.
// Arhiva ramane in memorie cat timp e montata; fisierele se citesc direct din ea.
void tarfs_init();
// Radacina implicita: monteaza modulul "initrd" pe "/"; 0 sau -errno (-ENOENT daca nu exista modulul)
int tarfs_mount_root();
