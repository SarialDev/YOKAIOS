#pragma once
#include "types.h"

// tmpfs (fisiere in pagini alocate la cerere) si devfs (dispozitive din HAL); ambele se inregistreaza ca FileSystemType
void tmpfs_init();
void devfs_init();

// Dupa initrd: devfs pe /dev, tmpfs pe /tmp (mode=1777) si pe /run (mode=0755). 0 sau primul -errno
int fs_mount_pseudo();
// Le desprinde in ordine inversa (testele cer un VFS fara sub-mount-uri); 0 sau primul -errno
int fs_umount_pseudo();

// /dev/random vine din RDRAND; false = fallback pe TSC (necriptografic)
bool devfs_has_rdrand();
