#pragma once
#include "types.h"
#include "fs/vfs/inode.h"

// ext4 read-only (T6.7): superblock, descriptori de grup pe 32/64 biti (si meta_bg), inode-uri cu extente sau
// harta de blocuri (ext2/ext3), directoare liniare si cu index htree, symlink-uri. Tip de sistem de fisiere "ext4";
// sursa montarii = numele unui dispozitiv bloc din HAL ("sda2"), optiuni: "ro", "noload" (monteaza chiar daca
// jurnalul cere recuperare; jurnalul nu se reda niciodata). Se monteaza intotdeauna read-only (scrierea e T6.8).
// Montarea refuza (-EINVAL) flag-urile INCOMPAT necunoscute sau nesuportate (compresie, inline_data, criptare,
// casefold, jurnal extern, dirdata), bigalloc si blocurile mai mari de 4 KiB.
// Datele trec prin cache-ul de blocuri al dispozitivului dat: monteaza partitia (sda2), nu si discul (sda).
void ext4_init();

// Monteaza dev ("sda2" sau "/dev/sda2") pe "/" read-only; 0 sau -errno. Folosit pentru cmdline root=
int ext4_mount_root(const char* dev);

// Pentru teste: cate cautari in director au folosit indexul htree / scanarea liniara, si daca directorul are index
void ext4_stats(uint64_t& htree_lookups, uint64_t& linear_lookups);
bool ext4_dir_indexed(const Inode* dir);
