#pragma once
#include "types.h"
#include "hal/block.h"

// JBD2 (T6.8): jurnalul ext4 in modul "ordered", format Linux (superblock v2, descriptori, commit, revoke, tag-uri
// pe 32/64 biti, checksum-uri CSUM_V3). Blocul jurnalului = blocul fs = 4 KiB = bloc din bcache.
//
// O singura tranzactie ruleaza o data: sistemul de fisiere isi modifica metadatele prin jbd2_get() (blocul ramane
// pinned in cache si NU e marcat murdar, deci bcache nu-l scrie), iar jbd2_commit():
//   1. scrie datele (ordered: blocurile de date murdare ale dispozitivului) si le da flush;
//   2. scrie in jurnal: superblock (s_start), revoke, descriptori + copiile blocurilor de metadate, flush;
//   3. scrie blocul de commit, flush;
//   4. checkpoint imediat: marcheaza murdare blocurile reale, le scrie si le da flush;
//   5. goleste jurnalul (s_start = 0, s_sequence++), flush.
// Dupa un crash intre 2 si 5 jbd2_recover() reia tranzactiile comise (si ignora cea fara commit).

struct Jbd2Map {
    // Blocul jurnalului jblk (0 = superblock-ul jurnalului) -> blocul fs; 0 sau -errno
    int (*map)(void* ctx, uint64_t jblk, uint64_t& phys);
    void* ctx;
};

struct Jbd2Stats {
    uint64_t commits;
    uint64_t blocks;                // blocuri de metadate jurnalizate
    uint64_t revokes;               // inregistrari revoke scrise
    uint64_t replayed_txns;
    uint64_t replayed_blocks;
    uint64_t revoked_skipped;       // blocuri sarite la replay din cauza unui revoke
    bool csum_v3;                   // jurnalul are checksum-uri (JBD2_FEATURE_INCOMPAT_CSUM_V3)
};

struct Journal;

// Citeste si valideaza superblock-ul jurnalului. err: -EINVAL daca formatul nu e suportat (checksum v1/v2, async
// commit, fast commit, jurnal extern / partajat, alt bloc decat fs_bs), -EIO la eroare de I/O
Journal* jbd2_open(BlockDevice* dev, const Jbd2Map& m, uint32_t fs_bs, int& err);
void jbd2_free(Journal* j);

// Jurnalul contine tranzactii nereluate (s_start != 0)
bool jbd2_has_log(Journal* j);
// Reia tranzactiile comise si goleste jurnalul. Blocurile se scriu prin bcache si se sincronizeaza
int jbd2_recover(Journal* j);

// Blocul fs blk in tranzactia curenta: pointer la cei 4096 de octeti din cache (valid pana la urmatorul commit).
// read = false: blocul e nou, se umple cu zerouri fara citire. nullptr la eroare de I/O sau tranzactie plina
uint8_t* jbd2_get(Journal* j, uint64_t blk, bool read);
// Blocul a fost eliberat: iese din tranzactie (continutul modificat se arunca) si, daca incape, se scrie un revoke
void jbd2_forget(Journal* j, uint64_t blk, bool revoke);
// Blocuri in tranzactia curenta
uint32_t jbd2_pending(Journal* j);
bool jbd2_dirty(Journal* j);
int jbd2_commit(Journal* j);
void jbd2_stats(Journal* j, Jbd2Stats& out);
uint32_t jbd2_capacity(Journal* j);
uint32_t jbd2_sequence(Journal* j);

// Pentru teste: apelat in punctele din commit (0 inainte de jurnal, 1 dupa blocurile jurnalului dar fara commit,
// 2 dupa blocul de commit, 3 la jumatatea checkpoint-ului, 4 dupa checkpoint, inainte de golirea jurnalului)
extern void (*jbd2_crash_hook)(int stage);
