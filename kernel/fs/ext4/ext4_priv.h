#pragma once
#include "types.h"
#include "hal/block.h"
#include "block/bcache.h"
#include "fs/vfs/inode.h"
#include "sync/mutex.h"

// Interne ext4: volum, noduri (inode-uri incarcate), citire de blocuri (inode.cpp), directoare si htree (dir.cpp),
// VFS si montare (ext4.cpp). Blocul fs are cel mult 4 KiB, deci incape intr-un singur bloc din bcache.
// Tot ce e aici se apeleaza cu mutex-ul volumului luat (Volume::lock), in afara de read/map, care citesc doar
// campuri neschimbate dupa incarcare

namespace ext4 {

const uint16_t SB_MAGIC = 0xEF53;
const uint32_t ROOT_INO = 2;

const uint32_t COMPAT_SPARSE_SUPER2 = 0x0200;
const uint32_t INCOMPAT_FILETYPE = 0x0002;
const uint32_t INCOMPAT_RECOVER = 0x0004;
const uint32_t INCOMPAT_META_BG = 0x0010;
const uint32_t INCOMPAT_EXTENTS = 0x0040;
const uint32_t INCOMPAT_64BIT = 0x0080;
const uint32_t INCOMPAT_MMP = 0x0100;
const uint32_t INCOMPAT_FLEX_BG = 0x0200;
const uint32_t INCOMPAT_EA_INODE = 0x0400;
const uint32_t INCOMPAT_CSUM_SEED = 0x2000;
const uint32_t INCOMPAT_LARGEDIR = 0x4000;
// Tot ce nu e aici (COMPRESSION, JOURNAL_DEV, DIRDATA, INLINE_DATA, ENCRYPT, CASEFOLD, biti necunoscuti) se refuza
const uint32_t INCOMPAT_SUPPORTED = INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_META_BG | INCOMPAT_EXTENTS |
                                    INCOMPAT_64BIT | INCOMPAT_MMP | INCOMPAT_FLEX_BG | INCOMPAT_EA_INODE |
                                    INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR;
const uint32_t RO_COMPAT_SPARSE_SUPER = 0x0001;
const uint32_t RO_COMPAT_BIGALLOC = 0x0200;

const uint32_t SBF_UNSIGNED_HASH = 0x0002;      // s_flags

const uint32_t INODE_INDEX_FL = 0x00001000;
const uint32_t INODE_EXTENTS_FL = 0x00080000;
const uint32_t INODE_INLINE_FL = 0x10000000;

const uint32_t S_IFSOCK_ = 0140000;

const uint32_t NODE_BUCKETS = 64;

struct Volume;

// Un inode incarcat; ramane in memorie pana la kill (read-only, deci nu se schimba)
struct Node {
    Inode* ino;
    Volume* vol;
    Node* next;                     // lantul din bucket
    uint32_t flags;                 // i_flags
    bool fast_symlink;              // tinta in i_block
    uint32_t blk[15];               // i_block: extente sau harta de blocuri
    // cursor de readdir (sub lock-ul volumului): urmatoarea intrare vizibila e rd_idx, la blocul rd_lblk, offset rd_off
    bool rd_valid;
    uint64_t rd_idx;
    uint32_t rd_lblk;
    uint32_t rd_off;
};

struct Volume {
    BlockDevice* dev;
    SuperBlock* sb;
    Mutex lock;
    uint32_t bs;                    // 1024, 2048 sau 4096
    uint32_t bs_shift;
    uint64_t blocks;                // blocuri in fs
    uint32_t first_data_block;
    uint32_t bpg;                   // blocuri pe grup
    uint32_t ipg;                   // inode-uri pe grup
    uint32_t inodes;
    uint32_t inode_size;
    uint32_t groups;
    uint32_t desc_size;
    uint32_t incompat;
    uint32_t ro_compat;
    uint8_t hash_version;           // implicit, din superblock
    bool hash_unsigned;
    uint32_t hash_seed[4];
    uint64_t* itable;               // inode_table al fiecarui grup (bloc)
    Node* hash[NODE_BUCKETS];
    Node* root;
};

inline uint16_t le16(const uint8_t* p){
    return (uint16_t)(p[0] | p[1] << 8);
}

inline uint32_t le32(const uint8_t* p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// ---- inode.cpp ----

// Octeti de la offset-ul off din fs (prin bcache)
int read_bytes(Volume* v, uint64_t off, void* buf, uint64_t len);
// Blocul fs blk, pinned in b (brelse(b) cand nu mai e nevoie); pointer la datele lui sau nullptr (EIO / in afara fs)
uint8_t* fsblk_get(Volume* v, uint64_t blk, Buf*& b);
// Blocul fizic al blocului logic lblk al nodului (0 = gaura: zerouri) si cate blocuri consecutive (cel mult maxrun,
// cel putin 1) au aceeasi stare. 0 sau -EIO
int map_block(Node* n, uint64_t lblk, uint64_t maxrun, uint64_t& phys, uint64_t& run);
// Citeste din fisier (golurile = zerouri), nu depaseste size
int64_t node_read(Node* n, uint64_t pos, void* buf, uint64_t len);

// ---- dir.cpp ----

// Numarul de inode al numelui din director; -ENOENT. Foloseste indexul htree cand directorul il are
int dir_lookup(Node* d, const char* name, uint32_t len, uint32_t& ino);
// A index-a intrare vizibila (fara "." si ".."): 1 = out completat, 0 = sfarsit, <0 = eroare
int dir_readdir(Node* d, uint64_t index, DirEntry& out);
extern uint64_t stat_htree;
extern uint64_t stat_linear;

}
