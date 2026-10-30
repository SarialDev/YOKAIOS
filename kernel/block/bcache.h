#pragma once
#include "types.h"
#include "hal/block.h"
#include "sync/mutex.h"

// Cache de blocuri de 4 KiB peste un BlockDevice: LRU, write-back (blocurile murdare se scriu la eviction,
// la bsync() sau periodic de task-ul "bflush"). Un singur Mutex serializeaza structura cache-ului (si I/O-ul
// facut sub el); continutul unui bloc pinned (refs > 0) il protejeaza apelantul. Cache-ul nu face coerenta
// intre un disc si partitiile lui: un fs foloseste ori sda, ori sda1, nu ambele.

const uint32_t BCACHE_BLOCK = 4096;

struct Buf {
    BlockDevice* dev;
    uint64_t blk;               // in unitati de 4 KiB de la inceputul dispozitivului
    uint8_t* data;              // BCACHE_BLOCK octeti
    int refs;
    bool valid;
    bool dirty;
    uint64_t dirty_ms;          // uptime_ms() la prima modificare
    Buf* hnext;
    Buf* lprev;
    Buf* lnext;
};

struct BcacheStats {
    uint64_t hits, misses, evictions, writebacks;
    uint32_t buffers, dirty, pinned;
};

class BufferCache {
public:
    explicit BufferCache(uint32_t max_buffers);
    // Bloc citit din dispozitiv (pinned); nullptr la eroare de I/O, blk in afara dispozitivului sau cache plin de blocuri pinned
    Buf* read(BlockDevice* dev, uint64_t blk);
    // Ca read(), dar fara citire: pentru blocuri care se suprascriu complet (continutul e nedefinit)
    Buf* get(BlockDevice* dev, uint64_t blk);
    void mark_dirty(Buf* b);
    void release(Buf* b);
    // Scrie blocurile murdare ale lui dev (nullptr = toate) si da flush dispozitivelor atinse; primul cod de eroare
    int sync(BlockDevice* dev);
    // sync + elimina blocurile lui dev din cache (nu pot fi pinned)
    int invalidate(BlockDevice* dev);
    // Scrie blocurile murdare de cel putin age_ms milisecunde; intoarce cate au fost scrise
    int writeback_aged(uint64_t age_ms);
    BcacheStats stats();
private:
    Buf* lookup(BlockDevice* dev, uint64_t blk);
    Buf* alloc_buf();
    int write_buf(Buf* b);
    void unlink_hash(Buf* b);
    void lru_remove(Buf* b);
    void lru_front(Buf* b);
    uint32_t bucket(BlockDevice* dev, uint64_t blk) const;

    Mutex mtx;
    Buf lru;                    // santinela: lru.lnext = cel mai recent folosit, lru.lprev = cel mai vechi
    Buf** table;
    uint32_t table_size;
    uint32_t max;
    uint32_t count;
    uint64_t hits, misses, evictions, writebacks;
};

// Instanta globala folosita de sisteme de fisiere
void bcache_init(uint32_t max_buffers);
BufferCache& bcache();
// Task kernel care scrie periodic blocurile murdare mai vechi de 5 s
void bcache_start_flusher();

inline Buf* bread(BlockDevice* d, uint64_t blk){ return bcache().read(d, blk); }
inline Buf* bget(BlockDevice* d, uint64_t blk){ return bcache().get(d, blk); }
inline void bdirty(Buf* b){ bcache().mark_dirty(b); }
inline void brelse(Buf* b){ bcache().release(b); }
inline int bsync(BlockDevice* d){ return bcache().sync(d); }
