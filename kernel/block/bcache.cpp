#include "block/bcache.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

BufferCache* global_cache;
const uint64_t FLUSH_AGE_MS = 5000;
const uint32_t FLUSH_PERIOD_MS = 1000;
const int MAX_SYNC_DEVS = 16;
const int MAX_HOOKS = 8;

struct Hook {
    SyncHookFn fn;
    void* arg;
};

Mutex hook_mtx("bcache-hooks");
Hook hooks[MAX_HOOKS];

void run_hooks(bool periodic){
    // Cu hook_mtx luat: bcache_del_hook asteapta sfarsitul unui hook in curs (hook-urile nu apeleaza bsync(nullptr))
    MutexGuard g(hook_mtx);
    for(int i = 0; i < MAX_HOOKS; i++){
        if(hooks[i].fn){
            hooks[i].fn(hooks[i].arg, periodic);
        }
    }
}

uint32_t sectors_per_block(BlockDevice* dev){
    return BCACHE_BLOCK / dev->block_size();
}

bool geometry_ok(BlockDevice* dev){
    uint32_t bs = dev->block_size();
    return bs >= 512 && bs <= BCACHE_BLOCK && BCACHE_BLOCK % bs == 0;
}

// Blocuri de 4 KiB intregi de la inceputul dispozitivului
uint64_t device_blocks(BlockDevice* dev){
    return dev->block_count() / sectors_per_block(dev);
}

}

BufferCache::BufferCache(uint32_t max_buffers)
    : mtx("bcache"), lru(), table(nullptr), table_size(0), max(max_buffers), count(0),
      hits(0), misses(0), evictions(0), writebacks(0){
    lru.lprev = lru.lnext = &lru;
    table_size = 16;
    while(table_size < max_buffers){
        table_size <<= 1;
    }
    table = (Buf**)kzalloc(sizeof(Buf*) * table_size);
    if(!table){
        table_size = 0;
        max = 0;
    }
}

uint32_t BufferCache::bucket(BlockDevice* dev, uint64_t blk) const {
    uint64_t h = (uint64_t)(uintptr_t)dev * 0x9E3779B97F4A7C15UL ^ blk * 0xC2B2AE3D27D4EB4FUL;
    return (uint32_t)(h >> 40) & (table_size - 1);
}

void BufferCache::lru_remove(Buf* b){
    b->lprev->lnext = b->lnext;
    b->lnext->lprev = b->lprev;
}

void BufferCache::lru_front(Buf* b){
    b->lnext = lru.lnext;
    b->lprev = &lru;
    lru.lnext->lprev = b;
    lru.lnext = b;
}

void BufferCache::unlink_hash(Buf* b){
    Buf** pp = &table[bucket(b->dev, b->blk)];
    while(*pp && *pp != b){
        pp = &(*pp)->hnext;
    }
    if(*pp){
        *pp = b->hnext;
    }
    b->hnext = nullptr;
}

Buf* BufferCache::lookup(BlockDevice* dev, uint64_t blk){
    for(Buf* b = table[bucket(dev, blk)]; b; b = b->hnext){
        if(b->dev == dev && b->blk == blk){
            return b;
        }
    }
    return nullptr;
}

// mtx tinut
int BufferCache::write_buf(Buf* b){
    uint32_t spb = sectors_per_block(b->dev);
    int err = b->dev->write(b->blk * spb, spb, b->data);
    if(!err){
        b->dirty = false;
        writebacks++;
    }
    return err;
}

// Un buffer liber: unul nou cat timp nu s-a atins limita, altfel cel mai vechi nepinned (scris intai daca e murdar)
Buf* BufferCache::alloc_buf(){
    if(count < max){
        Buf* b = (Buf*)kzalloc(sizeof(Buf));
        uint8_t* d = (uint8_t*)kmalloc(BCACHE_BLOCK);
        if(b && d){
            b->data = d;
            count++;
            lru_front(b);
            return b;
        }
        kfree(b);
        kfree(d);
    }
    for(Buf* b = lru.lprev; b != &lru; b = b->lprev){
        if(b->refs) {
            continue;
        }
        if(b->dirty && write_buf(b)){
            continue;
        }
        unlink_hash(b);
        b->valid = false;
        evictions++;
        lru_remove(b);
        lru_front(b);
        return b;
    }
    return nullptr;
}

Buf* BufferCache::get(BlockDevice* dev, uint64_t blk){
    if(!table || !geometry_ok(dev) || blk >= device_blocks(dev)){
        return nullptr;
    }
    MutexGuard g(mtx);
    Buf* b = lookup(dev, blk);
    if(b){
        hits++;
        b->refs++;
        lru_remove(b);
        lru_front(b);
        return b;
    }
    misses++;
    b = alloc_buf();
    if(!b){
        return nullptr;
    }
    b->dev = dev;
    b->blk = blk;
    b->refs = 1;
    b->valid = false;
    b->dirty = false;
    uint32_t h = bucket(dev, blk);
    b->hnext = table[h];
    table[h] = b;
    return b;
}

Buf* BufferCache::read(BlockDevice* dev, uint64_t blk){
    Buf* b = get(dev, blk);
    if(!b){
        return nullptr;
    }
    MutexGuard g(mtx);
    if(!b->valid){
        uint32_t spb = sectors_per_block(dev);
        if(dev->read(blk * spb, spb, b->data)){
            b->refs--;
            unlink_hash(b);
            return nullptr;
        }
        b->valid = true;
    }
    return b;
}

void BufferCache::mark_dirty(Buf* b){
    MutexGuard g(mtx);
    b->valid = true;
    if(!b->dirty){
        b->dirty = true;
        b->dirty_ms = uptime_ms();
    }
}

void BufferCache::release(Buf* b){
    MutexGuard g(mtx);
    if(b->refs <= 0){
        kprintf("bcache: release fara pin pe blocul %lu\n", b->blk);
        return;
    }
    b->refs--;
}

int BufferCache::sync(BlockDevice* dev){
    if(!dev){
        run_hooks(false);
    }
    MutexGuard g(mtx);
    BlockDevice* touched[MAX_SYNC_DEVS];
    int ntouched = 0;
    int result = 0;
    for(Buf* b = lru.lnext; b != &lru; b = b->lnext){
        if(!b->dirty || (dev && b->dev != dev)){
            continue;
        }
        int err = write_buf(b);
        if(err){
            result = result ? result : err;
            continue;
        }
        bool seen = false;
        for(int i = 0; i < ntouched; i++){
            seen = seen || touched[i] == b->dev;
        }
        if(!seen && ntouched < MAX_SYNC_DEVS){
            touched[ntouched++] = b->dev;
        }
    }
    if(dev && ntouched == 0){
        touched[ntouched++] = dev;
    }
    for(int i = 0; i < ntouched; i++){
        int err = touched[i]->flush();
        result = result ? result : err;
    }
    return result;
}

int BufferCache::invalidate(BlockDevice* dev){
    int err = sync(dev);
    if(err){
        return err;
    }
    MutexGuard g(mtx);
    for(Buf* b = lru.lnext; b != &lru; b = b->lnext){
        if(b->dev == dev && b->refs){
            return -EBUSY;
        }
    }
    Buf* b = lru.lnext;
    while(b != &lru){
        Buf* next = b->lnext;
        if(b->dev == dev){
            unlink_hash(b);
            lru_remove(b);
            kfree(b->data);
            kfree(b);
            count--;
        }
        b = next;
    }
    return 0;
}

int BufferCache::writeback_aged(uint64_t age_ms){
    MutexGuard g(mtx);
    uint64_t now = uptime_ms();
    int written = 0;
    for(Buf* b = lru.lnext; b != &lru; b = b->lnext){
        if(b->dirty && now - b->dirty_ms >= age_ms && write_buf(b) == 0){
            written++;
        }
    }
    return written;
}

BcacheStats BufferCache::stats(){
    MutexGuard g(mtx);
    BcacheStats s = {hits, misses, evictions, writebacks, count, 0, 0};
    for(Buf* b = lru.lnext; b != &lru; b = b->lnext){
        s.dirty += b->dirty;
        s.pinned += b->refs > 0;
    }
    return s;
}

bool bcache_add_hook(SyncHookFn fn, void* arg){
    MutexGuard g(hook_mtx);
    for(int i = 0; i < MAX_HOOKS; i++){
        if(!hooks[i].fn){
            hooks[i] = Hook{fn, arg};
            return true;
        }
    }
    return false;
}

void bcache_del_hook(void* arg){
    MutexGuard g(hook_mtx);
    for(int i = 0; i < MAX_HOOKS; i++){
        if(hooks[i].fn && hooks[i].arg == arg){
            hooks[i] = Hook{nullptr, nullptr};
        }
    }
}

void bcache_init(uint32_t max_buffers){
    if(!global_cache){
        global_cache = new BufferCache(max_buffers);
    }
}

BufferCache& bcache(){
    if(!global_cache){
        bcache_init(1024);              // 4 MiB; o tranzactie ext4 tine pinned pana la 160 de blocuri
    }
    return *global_cache;
}

namespace {

void flusher(void*){
    for(;;){
        sleep_ms(FLUSH_PERIOD_MS);
        run_hooks(true);
        bcache().writeback_aged(FLUSH_AGE_MS);
    }
}

}

void bcache_start_flusher(){
    Task* t = task_create_kernel("bflush", flusher, nullptr);
    if(t){
        sched_add(t);
    } else {
        kprintf("[WARN] bcache: fara task bflush, blocurile murdare se scriu doar la bsync\n");
    }
}
