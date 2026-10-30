#pragma once
#include "types.h"

// tmpfs (fisiere in pagini alocate la cerere) si devfs (dispozitive din HAL); ambele se inregistreaza ca FileSystemType
void tmpfs_init();
void devfs_init();
// procfs: /proc/{meminfo,uptime,cpuinfo,mounts,self,<pid>/{status,cmdline,maps}}, generat la citire
void procfs_init();

// Dupa initrd: devfs pe /dev, tmpfs pe /tmp (mode=1777) si pe /run (mode=0755), procfs pe /proc. 0 sau primul -errno
int fs_mount_pseudo();
// Le desprinde in ordine inversa (testele cer un VFS fara sub-mount-uri); 0 sau primul -errno
int fs_umount_pseudo();

// /dev/random vine din RDRAND; false = fallback pe TSC (necriptografic)
bool devfs_has_rdrand();
