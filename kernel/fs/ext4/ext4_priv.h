#pragma once
#include "types.h"
#include "hal/block.h"
#include "block/bcache.h"
#include "fs/vfs/inode.h"
#include "fs/jbd2/jbd2.h"
#include "sync/mutex.h"

// Interne ext4: volum, noduri (inode-uri incarcate), citire de blocuri (inode.cpp), directoare si htree (dir.cpp),
// VFS si montare (ext4.cpp). T6.8 adauga scrierea: tranzactii jbd2 (tx_*), alocare de blocuri si inode-uri
// (alloc.cpp), extente (extw.cpp), modificarea directoarelor (dirw.cpp) si checksum-uri (csum.cpp).
// Blocul fs are cel mult 4 KiB; scrierea cere exact 4 KiB (= bloc din bcache = bloc de jurnal).
// Tot ce e aici se apeleaza cu mutex-ul volumului luat (Volume::lock)

namespace ext4 {

const uint16_t SB_MAGIC = 0xEF53;
const uint32_t ROOT_INO = 2;
const uint32_t JOURNAL_INO = 8;

const uint32_t COMPAT_HAS_JOURNAL = 0x0004;
const uint32_t COMPAT_SPARSE_SUPER2 = 0x0200;
const uint32_t COMPAT_FAST_COMMIT = 0x0400;
const uint32_t COMPAT_ORPHAN_FILE = 0x1000;
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
const uint32_t RO_COMPAT_LARGE_FILE = 0x0002;
const uint32_t RO_COMPAT_HUGE_FILE = 0x0008;
const uint32_t RO_COMPAT_GDT_CSUM = 0x0010;
const uint32_t RO_COMPAT_DIR_NLINK = 0x0020;
const uint32_t RO_COMPAT_EXTRA_ISIZE = 0x0040;
const uint32_t RO_COMPAT_BIGALLOC = 0x0200;
const uint32_t RO_COMPAT_METADATA_CSUM = 0x0400;
// Scrierea: orice alt bit RO_COMPAT (quota, readonly, project, verity, orphan_present...) => montare read-only
const uint32_t RO_COMPAT_WRITABLE = RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | RO_COMPAT_HUGE_FILE |
                                    RO_COMPAT_GDT_CSUM | RO_COMPAT_DIR_NLINK | RO_COMPAT_EXTRA_ISIZE |
                                    RO_COMPAT_METADATA_CSUM;

const uint32_t SBF_UNSIGNED_HASH = 0x0002;      // s_flags
const uint16_t STATE_VALID = 0x0001;

const uint32_t INODE_INDEX_FL = 0x00001000;
const uint32_t INODE_HUGE_FILE_FL = 0x00040000;
const uint32_t INODE_EXTENTS_FL = 0x00080000;
const uint32_t INODE_INLINE_FL = 0x10000000;

const uint16_t BG_INODE_UNINIT = 1;
const uint16_t BG_BLOCK_UNINIT = 2;
const uint16_t BG_ITABLE_ZEROED = 4;

const uint32_t S_IFSOCK_ = 0140000;

const uint32_t NODE_BUCKETS = 64;
const uint32_t TX_SOFT_MAX = 112;               // dupa asta se comite inainte de o operatie noua

struct Volume;

// Un inode incarcat; ramane in memorie pana la kill
struct Node {
    Inode* ino;
    Volume* vol;
    Node* next;                     // lantul din bucket
    uint32_t flags;                 // i_flags
    bool fast_symlink;              // tinta in i_block
    uint32_t blk[15];               // i_block: extente sau harta de blocuri
    uint64_t blocks512;             // i_blocks (unitati de 512 octeti, inclusiv blocurile arborelui de extente)
    uint32_t gen;                   // i_generation (intra in checksum-ul inode-ului)
    uint32_t dtime;                 // i_dtime (la inode-uri orfane = urmatorul din lista)
    uint32_t file_acl;              // blocul de atribute extinse (doar eliberat la stergere)
    uint32_t extra_isize;           // 0 la inode-uri de 128 de octeti
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

    // ---- scriere (T6.8) ----
    bool rw;                        // montat cu scriere si jurnal
    bool csum;                      // metadata_csum
    bool gdt_csum;                  // uninit_bg cu crc16 (fara metadata_csum)
    uint32_t csum_seed;
    uint8_t uuid[16];
    uint8_t sbraw[1024];            // superblock-ul in memorie (sursa pentru ce se jurnalizeaza)
    bool sb_dirty;
    uint64_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_ino;
    uint32_t gen_ctr;
    uint64_t* gd_off;               // offset in octeti al descriptorului fiecarui grup
    const InodeOps* iops;
    Journal* jnl;
    Node* jnode;
    uint64_t last_commit_ms;
    uint64_t commit_ms;             // intervalul commit-ului periodic (optiunea commit=<s>, implicit 5 s)
    // Blocurile eliberate in tranzactia curenta nu se realoca pana la commit (altfel o cadere ar lasa un bloc al unui
    // fisier vechi, inca valid pe disc, suprascris cu date noi). Ca b_committed_data din Linux: la prima eliberare
    // dintr-un grup se pastreaza o copie a bitmap-ului de blocuri; balloc cere bitul liber in ambele
    uint8_t** freed_bm;             // [groups], nullptr = grupul nu are eliberari in tranzactia curenta
    uint32_t* freed_list;           // grupurile cu copie
    uint32_t nfreed;
    uint32_t orphans_done;
    bool nojournal_warned;
};

inline uint16_t le16(const uint8_t* p){
    return (uint16_t)(p[0] | p[1] << 8);
}

inline uint32_t le32(const uint8_t* p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

inline void put16(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

inline void put32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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
// Inode-ul ino (incarcat o singura data; ramane in memorie). allow_dead: accepta si inode-uri cu nlink 0 (orfane)
int node_load(Volume* v, uint32_t ino, Node*& out, bool allow_dead = false);
// Scrie campurile inode-ului in tabela de inode-uri, in tranzactie (+ checksum)
int inode_store(Node* n);
// Offset (octeti, in fs) al inode-ului ino
uint64_t inode_offset(Volume* v, uint32_t ino);
// Initializeaza in tranzactie inode-ul ino cu mode/flags date (restul zero), extent root gol daca flags are EXTENTS_FL
int inode_init(Volume* v, uint32_t ino, uint32_t mode, uint32_t flags, Node*& out);

// ---- csum.cpp ----

uint32_t csum_inode_seed(Volume* v, uint32_t ino, uint32_t gen);
// Checksum-ul bitmap-ului (blocuri / inode-uri) al grupului g in descriptor + csum-ul descriptorului
void gd_set_csums(Volume* v, uint32_t g, uint8_t* gd, const uint8_t* bbitmap, const uint8_t* ibitmap);
void gd_csum_update(Volume* v, uint32_t g, uint8_t* gd);
void sb_csum_update(Volume* v, uint8_t* sb1024);
void inode_csum_set(Volume* v, uint32_t ino, uint8_t* raw);
// Bloc de director (cu dirent tail) / nod dx / bloc de extente
void dirblock_csum_set(Node* d, uint8_t* blk);
void dxnode_csum_set(Node* d, uint8_t* blk, uint32_t count_off);
void extblock_csum_set(Node* n, uint8_t* blk);

// ---- alloc.cpp ----

uint8_t* tx_get(Volume* v, uint64_t blk, bool read);
// Fa loc pentru o operatie care modifica cel mult credits blocuri: comite tranzactia daca ar depasi limita
int tx_begin(Volume* v, uint32_t credits);
int tx_commit(Volume* v);
// Elibereaza copiile bitmap-urilor (dupa commit)
void freed_release(Volume* v);
// Scrie campurile superblock-ului (contoare) in tranzactie
int sb_flush(Volume* v);
// Blocul eliberat iese din tranzactie; metadata => revoke
void tx_forget(Volume* v, uint64_t blk, bool metadata);

// Descriptorul de grup g: copie (gd_read, out are cel putin 64 de octeti) sau pointer in tranzactie (gd_rw)
int gd_read(Volume* v, uint32_t g, uint8_t* out);
uint8_t* gd_rw(Volume* v, uint32_t g);
// Grupul g are copie a superblock-ului si a descriptorilor (sparse_super)
bool group_has_super(const uint8_t* sb, uint32_t g, uint32_t ro_compat);
// Aloca pana la want blocuri consecutive aproape de goal (0 = oriunde): first/got. -ENOSPC
int balloc(Volume* v, uint64_t goal, uint32_t want, uint64_t& first, uint32_t& got);
int bfree(Volume* v, uint64_t blk, uint64_t cnt, bool metadata);
// Inode nou aproape de grupul near_group. -ENOSPC
int ialloc(Volume* v, uint32_t near_group, bool isdir, uint32_t& ino);
int ifree(Volume* v, uint32_t ino, bool isdir);
// Recalculeaza contoarele volumului din descriptorii de grup (la mount)
void recount(Volume* v);

// ---- extw.cpp ----

// Initializeaza radacina arborelui de extente din i_block (header gol)
void ext_init_root(uint32_t* blk15);
// Adauga maparea lblk..lblk+len -> phys (nu trebuie sa se suprapuna cu ce exista). -ENOSPC, -EOPNOTSUPP, -EIO
int ext_insert(Node* n, uint32_t lblk, uint64_t phys, uint32_t len);
// Elimina maparile de la from_lblk incolo si elibereaza blocurile; cel mult budget extente per apel (more = a mai
// ramas de facut)
int ext_truncate(Node* n, uint32_t from_lblk, uint32_t budget, bool& more);
// Aloca pana la want blocuri consecutive pentru blocurile logice lblk.. (care sunt goluri) si le adauga in arbore;
// first/got = ce s-a alocat. Blocurile sunt zerouite (nu trebuie citite)
int file_alloc_range(Node* n, uint32_t lblk, uint32_t want, uint64_t& first, uint32_t& got);
// Elibereaza toate blocurile unui inode cu harta de blocuri (ext2/3)
int bmap_free_all(Node* n);

// ---- dir.cpp / dirw.cpp ----

// Numarul de inode al numelui din director; -ENOENT. Foloseste indexul htree cand directorul il are
int dir_lookup(Node* d, const char* name, uint32_t len, uint32_t& ino);
// A index-a intrare vizibila (fara "." si ".."): 1 = out completat, 0 = sfarsit, <0 = eroare
int dir_readdir(Node* d, uint64_t index, DirEntry& out);
extern uint64_t stat_htree;
extern uint64_t stat_linear;
// Hash-ul htree al numelui (versiunea ver, vezi dir.cpp)
bool dir_name_hash(Volume* v, uint32_t ver, const char* name, uint32_t len, uint32_t& out);
// Blocul logic lblk al directorului, pinned in b
uint8_t* dir_block(Node* d, uint32_t lblk, Buf*& b);

int dir_add(Node* d, const char* name, uint32_t len, uint32_t ino, uint8_t ftype);
int dir_remove(Node* d, const char* name, uint32_t len);
// Intrarea numita name primeste alt inode (rename peste un fisier existent): -ENOENT daca lipseste
int dir_set_ino(Node* d, const char* name, uint32_t len, uint32_t ino, uint8_t ftype);
int dir_set_dotdot(Node* d, uint32_t parent);
// Continutul primului bloc al unui director nou: "." si ".."
int dir_init_block(Node* d, uint8_t* blk, uint32_t self, uint32_t parent);
bool dir_is_empty(Node* d, int& err);
uint8_t ftype_of_mode(uint32_t mode);


// ---- ext4.cpp ----

// Elibereaza datele unui inode fara legaturi, il scoate din lista de orfani si ii elibereaza numarul
int node_destroy(Node* n);
int orphan_add(Node* n);
int orphan_del(Node* n);

}
