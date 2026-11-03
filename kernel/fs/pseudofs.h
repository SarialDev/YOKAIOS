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

struct Inode;
// true = nodul /dev/console sau /dev/tty (terminalul): ioctl-urile tty merg numai pe el
bool devfs_is_console(const Inode* i);

// /dev/fb0 (T10.1). Lungimea mapabila cu mmap (0 = nodul nu se poate mapa)
uint64_t devfs_mmap_len(const Inode* i);
// Adresa fizica a paginii de la off (aliniat la pagina) si bitii de cache ai PTE-ului; -ENODEV / -ENXIO
int64_t devfs_mmap_phys(const Inode* i, uint64_t off, uint64_t& pte_cache);
// O zona VMA pe nod apare (get) / dispare: prima mapare trece ecranul in modul grafic, ultima il reda consolei
void devfs_mmap_ref(const Inode* i, bool get);
// ioctl pe un nod din /dev care nu e terminal (FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO); -ENOTTY daca nu stie
int64_t devfs_ioctl(Inode* i, uint64_t cmd, uint64_t arg);
