#pragma once
#include "types.h"
#include "hal/block.h"
#include "block/bcache.h"
#include "fs/vfs/inode.h"
#include "sync/mutex.h"

// Interne FAT32: volum, cache de FAT, lanturi de clustere (fat.cpp), directoare si nume (dir.cpp), VFS (fat32.cpp).
// Tot ce e aici se apeleaza cu mutex-ul volumului luat (Volume::lock)

namespace fat32 {

const uint32_t FAT_MASK = 0x0FFFFFFF;
const uint32_t FAT_EOC = 0x0FFFFFFF;            // ce scriem la sfarsit de lant
const uint32_t FAT_EOC_MIN = 0x0FFFFFF8;
const uint32_t FAT_BAD = 0x0FFFFFF7;
const uint32_t FAT_CLEAN = 0x08000000;          // FAT[1]: volumul a fost demontat curat

const uint8_t ATTR_RO = 0x01;
const uint8_t ATTR_VOLID = 0x08;
const uint8_t ATTR_DIR = 0x10;
const uint8_t ATTR_ARCHIVE = 0x20;
const uint8_t ATTR_LFN = 0x0F;

const uint32_t ENT_SIZE = 32;
const uint32_t DIR_MAX_BYTES = 2u << 20;        // un director are cel mult 65536 intrari
const uint32_t NAME16_MAX = 255;                // unitati UTF-16 intr-un nume
const uint32_t FAT_WINDOW = 4096;
const uint32_t FAT_WINDOWS = 16;

struct Node;

// O fereastra de 4 KiB din tabela FAT, tinuta in memorie (citire prin ea, scriere si prin cache-ul de blocuri)
struct FatWindow {
    uint32_t idx;
    bool valid;
    uint64_t stamp;
    uint8_t* data;
};

struct Volume {
    BlockDevice* dev;
    SuperBlock* sb;
    Mutex lock;
    Volume* next;
    bool readonly;
    uint32_t bps;                   // octeti pe sector
    uint32_t spc;                   // sectoare pe cluster
    uint32_t cluster_bytes;
    uint32_t nfats;
    uint32_t active_fat;            // FAT-ul citit; scrierea merge in toate daca mirror
    bool mirror;
    uint64_t fat_off;               // octeti de la inceputul volumului
    uint64_t fat_bytes;             // un FAT
    uint64_t data_off;
    uint32_t clusters;              // clustere de date: numerele 2 .. clusters + 1
    uint32_t root_cluster;
    uint32_t fsinfo_sector;         // 0 = fara FSInfo
    uint32_t free_count;
    uint32_t next_free;
    bool was_dirty;                 // marcat murdar deja la montare
    bool dirty_set;                 // l-am marcat noi
    uint64_t win_clock;
    FatWindow win[FAT_WINDOWS];
    Node* nodes;                    // inode-urile vii
    Node* root;
};

// Un fisier sau director cunoscut: legat de intrarea lui din directorul parinte
struct Node {
    Node* prev;
    Node* next;
    Volume* vol;
    Inode* ino;
    uint32_t first;                 // primul cluster (0 = gol); la director, inceputul fluxului de intrari
    uint32_t dir_first;             // primul cluster al directorului parinte (0 la radacina)
    uint32_t ent_off;               // offset-ul intrarii scurte in fluxul parintelui
    uint32_t lfn_off;               // offset-ul primei intrari de nume lung (== ent_off fara LFN)
    bool dead;                      // intrarea a fost stearsa
    uint8_t attr;
    uint32_t c_idx;                 // ultimul cluster atins: indexul si numarul lui (0 = necunoscut)
    uint32_t c_clu;
    bool rd_valid;                  // readdir: dupa rd_vis intrari vizibile suntem la rd_off
    uint64_t rd_vis;
    uint32_t rd_off;
};

// O intrare de director decodata. name = numele afisat (LFN daca exista si e valid, altfel cel scurt)
struct Entry {
    uint8_t raw[ENT_SIZE];
    uint16_t name[NAME16_MAX + 1];
    uint32_t nlen;
    uint16_t alias[13];
    uint32_t alen;
    uint32_t lfn_off;
    uint32_t ent_off;
    bool has_lfn;
};

struct DirIter {
    Volume* v;
    uint32_t first;
    uint32_t idx;
    uint32_t clu;
};

// ---- fat.cpp ----
// Octeti de volum prin cache-ul de blocuri; -EIO la eroare
int vol_rw(Volume* v, uint64_t off, void* buf, uint64_t len, bool write);
int vol_zero(Volume* v, uint64_t off, uint64_t len);
// Prima modificare: marcheaza volumul murdar (bitul din boot sector si FAT[1])
int vol_begin_write(Volume* v);
int vol_flush_state(Volume* v);                 // FSInfo + bitii "curat", la umount
uint64_t clu_offset(const Volume* v, uint32_t c);
bool clu_valid(const Volume* v, uint32_t c);
int fat_get(Volume* v, uint32_t c, uint32_t& val);
int fat_set(Volume* v, uint32_t c, uint32_t val);
// Urmatorul cluster din lant; next = 0 la sfarsit; -EIO la lant stricat
int fat_next(Volume* v, uint32_t c, uint32_t& next);
// Un cluster liber, zerorizat, adaugat dupa prev (0 = lant nou)
int clu_alloc(Volume* v, uint32_t prev, uint32_t& out);
void clu_free_chain(Volume* v, uint32_t first);
void fat_cache_drop(Volume* v);

// ---- dir.cpp ----
int utf8_to_utf16(const char* s, uint32_t len, uint16_t* out, uint32_t cap, uint32_t& n);
uint32_t utf16_to_utf8(const uint16_t* s, uint32_t n, char* out, uint32_t cap);
bool name_valid(const uint16_t* name, uint32_t n);
bool name_equal(const uint16_t* a, uint32_t na, const uint16_t* b, uint32_t nb);

void fat_time_encode(int64_t epoch, uint16_t& date, uint16_t& time);
Timespec fat_time_decode(uint16_t date, uint16_t time);

void it_init(DirIter& it, Volume* v, uint32_t first);
// 0 = intrarea la off exista, 1 = dincolo de lant, <0 eroare
int dir_read_raw(DirIter& it, uint32_t off, uint8_t* e);
int dir_write_raw(DirIter& it, uint32_t off, const uint8_t* e);
// Urmatoarea intrare valida de la off (off avanseaza dupa ea); 1 = gasita, 0 = sfarsit, <0 eroare
int dir_next(DirIter& it, uint32_t& off, Entry& e);
int dir_find(Volume* v, uint32_t dir_first, const uint16_t* name, uint32_t n, Entry& e);
// 1 = nu contine nimic in afara de "." si ".."
int dir_empty(Volume* v, uint32_t dir_first);
// Scrie numele (LFN daca trebuie) si intrarea scurta; raw vine cu atribut, timpi, cluster si dimensiune completate
// si se intoarce cu numele scurt completat
int dir_add(Volume* v, uint32_t dir_first, const uint16_t* name, uint32_t n, uint8_t* raw, uint32_t& lfn_off,
            uint32_t& ent_off);
// Marcheaza sterse intrarile de la lfn_off la ent_off inclusiv; saved primeste primii octeti (pentru dir_restore)
int dir_delete(Volume* v, uint32_t dir_first, uint32_t lfn_off, uint32_t ent_off);
int dir_undelete(Volume* v, uint32_t dir_first, uint32_t lfn_off, uint32_t ent_off, const uint8_t* saved);
int dir_save_marks(Volume* v, uint32_t dir_first, uint32_t lfn_off, uint32_t ent_off, uint8_t* saved);

}
