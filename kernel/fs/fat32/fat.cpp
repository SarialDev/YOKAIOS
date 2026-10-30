#include "fs/fat32/fat32_priv.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace fat32 {

namespace {

uint32_t le32(const uint8_t* p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void put32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Fereastra numarul idx din FAT-ul activ, incarcata la nevoie (LRU)
int fat_window(Volume* v, uint32_t idx, FatWindow*& out){
    for(uint32_t i = 0; i < FAT_WINDOWS; i++){
        FatWindow* w = &v->win[i];
        if(w->valid && w->idx == idx){
            w->stamp = ++v->win_clock;
            out = w;
            return 0;
        }
    }
    FatWindow* victim = &v->win[0];
    for(uint32_t i = 0; i < FAT_WINDOWS; i++){
        FatWindow* w = &v->win[i];
        if(!w->valid){
            victim = w;
            break;
        }
        if(w->stamp < victim->stamp){
            victim = w;
        }
    }
    uint64_t base = (uint64_t)idx * FAT_WINDOW;
    uint64_t len = v->fat_bytes - base < FAT_WINDOW ? v->fat_bytes - base : FAT_WINDOW;
    victim->valid = false;
    memset(victim->data, 0, FAT_WINDOW);
    int err = vol_rw(v, v->fat_off + (uint64_t)v->active_fat * v->fat_bytes + base, victim->data, len, false);
    if(err){
        return err;
    }
    victim->idx = idx;
    victim->valid = true;
    victim->stamp = ++v->win_clock;
    out = victim;
    return 0;
}

}

int vol_rw(Volume* v, uint64_t off, void* buf, uint64_t len, bool write){
    if(write && v->readonly){
        return -EROFS;
    }
    uint8_t* p = (uint8_t*)buf;
    while(len){
        uint64_t blk = off / BCACHE_BLOCK;
        uint32_t bo = (uint32_t)(off % BCACHE_BLOCK);
        uint64_t n = BCACHE_BLOCK - bo;
        if(n > len){
            n = len;
        }
        Buf* b = write && n == BCACHE_BLOCK ? bget(v->dev, blk) : bread(v->dev, blk);
        if(!b){
            return -EIO;
        }
        if(write){
            memcpy(b->data + bo, p, n);
            bdirty(b);
        } else {
            memcpy(p, b->data + bo, n);
        }
        brelse(b);
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

int vol_zero(Volume* v, uint64_t off, uint64_t len){
    if(v->readonly){
        return -EROFS;
    }
    while(len){
        uint64_t blk = off / BCACHE_BLOCK;
        uint32_t bo = (uint32_t)(off % BCACHE_BLOCK);
        uint64_t n = BCACHE_BLOCK - bo;
        if(n > len){
            n = len;
        }
        Buf* b = n == BCACHE_BLOCK ? bget(v->dev, blk) : bread(v->dev, blk);
        if(!b){
            return -EIO;
        }
        memset(b->data + bo, 0, n);
        bdirty(b);
        brelse(b);
        off += n;
        len -= n;
    }
    return 0;
}

uint64_t clu_offset(const Volume* v, uint32_t c){
    return v->data_off + (uint64_t)(c - 2) * v->cluster_bytes;
}

bool clu_valid(const Volume* v, uint32_t c){
    return c >= 2 && c < v->clusters + 2;
}

int fat_get(Volume* v, uint32_t c, uint32_t& val){
    if(c >= v->clusters + 2){
        return -EIO;
    }
    FatWindow* w;
    int err = fat_window(v, (uint32_t)((uint64_t)c * 4 / FAT_WINDOW), w);
    if(err){
        return err;
    }
    val = le32(w->data + (uint64_t)c * 4 % FAT_WINDOW) & FAT_MASK;
    return 0;
}

int fat_set(Volume* v, uint32_t c, uint32_t val){
    if(c >= v->clusters + 2){
        return -EIO;
    }
    FatWindow* w;
    int err = fat_window(v, (uint32_t)((uint64_t)c * 4 / FAT_WINDOW), w);
    if(err){
        return err;
    }
    uint8_t* p = w->data + (uint64_t)c * 4 % FAT_WINDOW;
    // Cei 4 biti de sus sunt rezervati si se pastreaza
    uint32_t nv = (le32(p) & ~FAT_MASK) | (val & FAT_MASK);
    put32(p, nv);
    uint8_t raw[4];
    put32(raw, nv);
    for(uint32_t f = 0; f < v->nfats; f++){
        if(!v->mirror && f != v->active_fat){
            continue;
        }
        err = vol_rw(v, v->fat_off + (uint64_t)f * v->fat_bytes + (uint64_t)c * 4, raw, 4, true);
        if(err){
            return err;
        }
    }
    return 0;
}

int fat_next(Volume* v, uint32_t c, uint32_t& next){
    uint32_t val;
    int err = fat_get(v, c, val);
    if(err){
        return err;
    }
    if(val >= FAT_EOC_MIN){
        next = 0;
        return 0;
    }
    if(!clu_valid(v, val)){
        return -EIO;                    // liber, defect sau in afara volumului: lant stricat
    }
    next = val;
    return 0;
}

int clu_alloc(Volume* v, uint32_t prev, uint32_t& out){
    if(v->free_count == 0){
        return -ENOSPC;
    }
    uint32_t total = v->clusters;
    uint32_t start = v->next_free;
    if(start < 2 || start >= total + 2){
        start = 2;
    }
    for(uint32_t i = 0; i < total; i++){
        uint32_t c = start + i;
        if(c >= total + 2){
            c -= total;
        }
        uint32_t val;
        int err = fat_get(v, c, val);
        if(err){
            return err;
        }
        if(val != 0){
            continue;
        }
        err = fat_set(v, c, FAT_EOC);
        if(!err){
            err = vol_zero(v, clu_offset(v, c), v->cluster_bytes);
        }
        if(!err && prev){
            err = fat_set(v, prev, c);
        }
        if(err){
            fat_set(v, c, 0);
            return err;
        }
        if(v->free_count != 0xFFFFFFFFu){
            v->free_count--;
        }
        v->next_free = c + 1;
        out = c;
        return 0;
    }
    return -ENOSPC;
}

void clu_free_chain(Volume* v, uint32_t first){
    uint32_t guard = v->clusters;
    for(uint32_t c = first; clu_valid(v, c) && guard--;){
        uint32_t val;
        if(fat_get(v, c, val)){
            return;
        }
        if(val == 0){
            return;                     // deja liber: lant stricat, ne oprim
        }
        if(fat_set(v, c, 0)){
            return;
        }
        if(v->free_count != 0xFFFFFFFFu){
            v->free_count++;
        }
        if(c < v->next_free){
            v->next_free = c;
        }
        c = val >= FAT_EOC_MIN ? 0 : val;
    }
}

void fat_cache_drop(Volume* v){
    for(uint32_t i = 0; i < FAT_WINDOWS; i++){
        v->win[i].valid = false;
    }
}

// Octetul 0x41 din boot sector (bit 0 = murdar) si FAT[1] (bitul 27 = curat)
int vol_begin_write(Volume* v){
    if(v->readonly){
        return -EROFS;
    }
    if(v->dirty_set){
        return 0;
    }
    uint8_t b;
    int err = vol_rw(v, 0x41, &b, 1, false);
    if(err){
        return err;
    }
    b |= 1;
    err = vol_rw(v, 0x41, &b, 1, true);
    if(err){
        return err;
    }
    uint32_t f1;
    err = fat_get(v, 1, f1);
    if(!err){
        err = fat_set(v, 1, f1 & ~FAT_CLEAN);
    }
    if(err){
        return err;
    }
    v->dirty_set = true;
    return 0;
}

int vol_flush_state(Volume* v){
    if(v->readonly || !v->dirty_set){
        return 0;
    }
    int first = 0;
    if(v->fsinfo_sector){
        uint8_t s[512];
        uint64_t off = (uint64_t)v->fsinfo_sector * v->bps;
        int err = vol_rw(v, off + 488, s, 8, false);
        if(!err){
            put32(s, v->free_count);
            put32(s + 4, v->next_free);
            err = vol_rw(v, off + 488, s, 8, true);
        }
        first = err;
    }
    if(!v->was_dirty){
        uint32_t f1;
        int err = fat_get(v, 1, f1);
        if(!err){
            err = fat_set(v, 1, f1 | FAT_CLEAN);
        }
        uint8_t b;
        if(!err){
            err = vol_rw(v, 0x41, &b, 1, false);
        }
        if(!err){
            b &= (uint8_t)~1;
            err = vol_rw(v, 0x41, &b, 1, true);
        }
        if(err && !first){
            first = err;
        }
    }
    return first;
}

}
