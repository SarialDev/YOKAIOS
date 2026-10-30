#pragma once
#include "types.h"
#include "fs/vfs/inode.h"

// ext4 (T6.7 citire, T6.8 scriere cu jurnal JBD2): superblock, descriptori de grup pe 32/64 biti (si meta_bg), inode-uri cu extente sau
// harta de blocuri (ext2/ext3), directoare liniare si cu index htree, symlink-uri. Tip de sistem de fisiere "ext4";
// sursa montarii = numele unui dispozitiv bloc din HAL ("sda2"), optiuni: "ro", "rw", "noload" (monteaza read-only
// chiar daca jurnalul cere recuperare; jurnalul nu se reda). Fara MS_RDONLY / "ro" / "noload" volumul se monteaza cu
// scriere daca se poate (blocuri de 4 KiB, extente, filetype, jurnal intern JBD2 CSUM_V3 sau fara checksum, fara
// meta_bg / mmp / fast_commit / orphan_file / inline_data): jurnalul se reia la montare (inclusiv inode-urile
// orfane), metadatele se scriu in tranzactii jbd2 (mod ordered), commit la 5 s (optiunea "commit=<s>"), la sync
// si la umount. Altfel montarea cade pe read-only si spune motivul. Fisierele cu harta de blocuri (ext2/3) se citesc
// si se pot sterge, nu se scriu (-EOPNOTSUPP); la fel extentele neinitializate (fallocate).
// Montarea refuza (-EINVAL) flag-urile INCOMPAT necunoscute sau nesuportate (compresie, inline_data, criptare,
// casefold, jurnal extern, dirdata), bigalloc si blocurile mai mari de 4 KiB.
// Datele trec prin cache-ul de blocuri al dispozitivului dat: monteaza partitia (sda2), nu si discul (sda).
void ext4_init();

// Monteaza dev ("sda2" sau "/dev/sda2") pe "/"; rw = cu scriere (cmdline "rw"); 0 sau -errno. Folosit pentru root=
int ext4_mount_root(const char* dev, bool rw);

struct Ext4Info {
    bool rw;
    uint64_t free_blocks;
    uint32_t free_inodes;
    uint64_t commits;               // tranzactii comise de la montare
    uint64_t journaled_blocks;
    uint64_t revokes;
    uint64_t replayed_txns;         // reluate la montare
    uint64_t replayed_blocks;
    uint64_t replay_revoked;        // blocuri sarite la replay din cauza unui revoke
    bool journal_csum;              // jurnal cu checksum-uri v3
    uint32_t sequence;              // numarul tranzactiei care urmeaza
    uint32_t pending;               // blocuri in tranzactia curenta
};
// Informatii despre volumul unui inode ext4; false daca inode-ul nu e ext4
bool ext4_info(const Inode* any, Ext4Info& out);
// Comite acum tranzactia curenta (ca sync pe acest volum)
int ext4_commit(const Inode* any);

// Pentru teste: cate cautari in director au folosit indexul htree / scanarea liniara, si daca directorul are index
void ext4_stats(uint64_t& htree_lookups, uint64_t& linear_lookups);
bool ext4_dir_indexed(const Inode* dir);
