#include "fs/ext4/ext4_priv.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "drivers/rtc.h"
#include "time/clock.h"
#include "uapi/syscall_nr.h"

namespace ext4 {

namespace {

uint64_t min64(uint64_t a, uint64_t b){
    return a < b ? a : b;
}

bool bit_test(const uint8_t* bm, uint32_t i){
    return (bm[i >> 3] >> (i & 7)) & 1;
}

void bit_set(uint8_t* bm, uint32_t i){
    bm[i >> 3] = (uint8_t)(bm[i >> 3] | 1u << (i & 7));
}

void bit_clear(uint8_t* bm, uint32_t i){
    bm[i >> 3] = (uint8_t)(bm[i >> 3] & ~(1u << (i & 7)));
}

// ---- campuri ale descriptorului de grup ----

bool wide(const Volume* v){
    return v->desc_size >= 64;
}

uint64_t gd_bb(const Volume* v, const uint8_t* gd){
    return le32(gd) | (wide(v) ? (uint64_t)le32(gd + 0x20) << 32 : 0);
}

uint64_t gd_ib(const Volume* v, const uint8_t* gd){
    return le32(gd + 4) | (wide(v) ? (uint64_t)le32(gd + 0x24) << 32 : 0);
}

uint64_t gd_it(const Volume* v, const uint8_t* gd){
    return le32(gd + 8) | (wide(v) ? (uint64_t)le32(gd + 0x28) << 32 : 0);
}

uint32_t gd_get32(const Volume* v, const uint8_t* gd, uint32_t lo, uint32_t hi){
    return le16(gd + lo) | (wide(v) ? (uint32_t)le16(gd + hi) << 16 : 0);
}

void gd_put32(const Volume* v, uint8_t* gd, uint32_t lo, uint32_t hi, uint32_t val){
    put16(gd + lo, val & 0xFFFF);
    if(wide(v)){
        put16(gd + hi, val >> 16);
    }
}

uint32_t gd_free_blk(const Volume* v, const uint8_t* gd){
    return gd_get32(v, gd, 0x0C, 0x2C);
}

uint32_t gd_free_ino(const Volume* v, const uint8_t* gd){
    return gd_get32(v, gd, 0x0E, 0x2E);
}

uint32_t gd_dirs(const Volume* v, const uint8_t* gd){
    return gd_get32(v, gd, 0x10, 0x30);
}

uint32_t gd_unused(const Volume* v, const uint8_t* gd){
    return gd_get32(v, gd, 0x1C, 0x32);
}

bool has_gd_csum(const Volume* v){
    return v->csum || v->gdt_csum;
}

// Copia bitmap-ului grupului g de dinaintea primei eliberari din tranzactia curenta
int freed_snapshot(Volume* v, uint32_t g, const uint8_t* bm){
    if(!v->freed_bm){
        v->freed_bm = (uint8_t**)kzalloc(sizeof(uint8_t*) * v->groups);
        v->freed_list = (uint32_t*)kmalloc(sizeof(uint32_t) * v->groups);
        if(!v->freed_bm || !v->freed_list){
            kfree(v->freed_bm);
            kfree(v->freed_list);
            v->freed_bm = nullptr;
            v->freed_list = nullptr;
            return -ENOMEM;
        }
    }
    if(v->freed_bm[g]){
        return 0;
    }
    uint8_t* c = (uint8_t*)kmalloc(v->bs);
    if(!c){
        return -ENOMEM;
    }
    memcpy(c, bm, v->bs);
    v->freed_bm[g] = c;
    v->freed_list[v->nfreed++] = g;
    return 0;
}

uint32_t itable_blocks(const Volume* v){
    return (uint32_t)(((uint64_t)v->ipg * v->inode_size + v->bs - 1) / v->bs);
}

// Bitmap-ul de blocuri al unui grup BLOCK_UNINIT: metadatele cunoscute + marginea (ca in mke2fs)
void build_uninit_bitmap(Volume* v, uint32_t g, uint8_t* bm){
    memset(bm, 0, v->bs);
    uint64_t gstart = v->first_data_block + (uint64_t)g * v->bpg;
    uint64_t gend = min64(gstart + v->bpg, v->blocks);
    if(group_has_super(v->sbraw, g, v->ro_compat)){
        uint32_t gdt_blocks = (uint32_t)(((uint64_t)v->groups * v->desc_size + v->bs - 1) / v->bs);
        uint32_t reserved = le16(v->sbraw + 0xCE);
        uint64_t n = 1 + gdt_blocks + reserved;
        for(uint64_t b = gstart; b < gstart + n && b < gend; b++){
            bit_set(bm, (uint32_t)(b - gstart));
        }
    }
    uint32_t itb = itable_blocks(v);
    for(uint32_t h = 0; h < v->groups; h++){
        uint8_t gd[64];
        if(gd_read(v, h, gd)){
            continue;
        }
        uint64_t bb = gd_bb(v, gd), ib = gd_ib(v, gd), it = gd_it(v, gd);
        if(bb >= gstart && bb < gend){
            bit_set(bm, (uint32_t)(bb - gstart));
        }
        if(ib >= gstart && ib < gend){
            bit_set(bm, (uint32_t)(ib - gstart));
        }
        for(uint64_t b = it > gstart ? it : gstart; b < it + itb && b < gend; b++){
            bit_set(bm, (uint32_t)(b - gstart));
        }
    }
    for(uint64_t i = gend - gstart; i < (uint64_t)v->bs * 8; i++){
        bit_set(bm, (uint32_t)i);
    }
}

// Bitmap-ul de blocuri al grupului g in tranzactie; gd = descriptorul in tranzactie
uint8_t* bbitmap_rw(Volume* v, uint32_t g, uint8_t* gd){
    uint16_t flags = le16(gd + 0x12);
    uint64_t blk = gd_bb(v, gd);
    if(has_gd_csum(v) && (flags & BG_BLOCK_UNINIT)){
        uint8_t* bm = tx_get(v, blk, false);
        if(!bm){
            return nullptr;
        }
        build_uninit_bitmap(v, g, bm);
        put16(gd + 0x12, flags & ~BG_BLOCK_UNINIT);
        gd_set_csums(v, g, gd, bm, nullptr);
        return bm;
    }
    return tx_get(v, blk, true);
}

uint8_t* ibitmap_rw(Volume* v, uint8_t* gd, bool& was_uninit){
    uint16_t flags = le16(gd + 0x12);
    was_uninit = has_gd_csum(v) && (flags & BG_INODE_UNINIT);
    return tx_get(v, gd_ib(v, gd), !was_uninit);
}

}

bool group_has_super(const uint8_t* sb, uint32_t g, uint32_t ro_compat){
    if(le32(sb + 0x5C) & COMPAT_SPARSE_SUPER2){
        return g == 0 || g == le32(sb + 0x24C) || g == le32(sb + 0x250);
    }
    if(!(ro_compat & RO_COMPAT_SPARSE_SUPER) || g <= 1){
        return true;
    }
    if(!(g & 1)){
        return false;
    }
    for(uint32_t base = 3; base <= 7; base += 2){
        uint64_t x = base;
        while(x < g){
            x *= base;
        }
        if(x == g){
            return true;
        }
    }
    return false;
}

// ---- tranzactii ----

uint8_t* tx_get(Volume* v, uint64_t blk, bool read){
    if(blk >= v->blocks || !v->jnl){
        return nullptr;
    }
    return jbd2_get(v->jnl, blk, read);
}

void tx_forget(Volume* v, uint64_t blk, bool metadata){
    jbd2_forget(v->jnl, blk, metadata);
}

int sb_flush(Volume* v){
    if(!v->sb_dirty){
        return 0;
    }
    uint8_t* b = tx_get(v, 0, true);
    if(!b){
        return -ENOSPC;
    }
    put32(v->sbraw + 0x0C, (uint32_t)v->free_blocks);
    if(v->incompat & INCOMPAT_64BIT){
        put32(v->sbraw + 0x158, (uint32_t)(v->free_blocks >> 32));
    }
    put32(v->sbraw + 0x10, v->free_inodes);
    put32(v->sbraw + 0x30, (uint32_t)wall_time());
    sb_csum_update(v, v->sbraw);
    memcpy(b + 1024, v->sbraw, 1024);
    v->sb_dirty = false;
    return 0;
}

void freed_release(Volume* v){
    for(uint32_t i = 0; i < v->nfreed; i++){
        kfree(v->freed_bm[v->freed_list[i]]);
        v->freed_bm[v->freed_list[i]] = nullptr;
    }
    v->nfreed = 0;
}

int tx_commit(Volume* v){
    int rc = sb_flush(v);
    if(!rc){
        rc = jbd2_commit(v->jnl);
    }
    freed_release(v);
    v->last_commit_ms = uptime_ms();
    if(rc){
        kprintf("ext4: commit esuat (%d), volumul trece pe read-only\n", rc);
        v->rw = false;
        v->sb->flags |= MS_RDONLY;
    }
    return rc;
}

int tx_begin(Volume* v, uint32_t credits){
    if(!v->rw){
        return -EROFS;
    }
    if(jbd2_pending(v->jnl) + credits > TX_SOFT_MAX){
        int rc = tx_commit(v);
        if(rc){
            return rc;
        }
    }
    return 0;
}

// ---- descriptori de grup ----

int gd_read(Volume* v, uint32_t g, uint8_t* out){
    if(g >= v->groups){
        return -EIO;
    }
    uint8_t tmp[64];
    uint32_t take = v->desc_size < 64 ? v->desc_size : 64;
    int err = read_bytes(v, v->gd_off[g], tmp, take);
    if(err){
        return err;
    }
    memset(out, 0, 64);
    memcpy(out, tmp, take);
    return 0;
}

uint8_t* gd_rw(Volume* v, uint32_t g){
    if(g >= v->groups){
        return nullptr;
    }
    uint64_t off = v->gd_off[g];
    uint8_t* b = tx_get(v, off >> v->bs_shift, true);
    return b ? b + (off & (v->bs - 1)) : nullptr;
}

void recount(Volume* v){
    uint64_t fb = 0;
    uint64_t fi = 0;
    for(uint32_t g = 0; g < v->groups; g++){
        uint8_t gd[64];
        if(gd_read(v, g, gd)){
            continue;
        }
        fb += gd_free_blk(v, gd);
        fi += gd_free_ino(v, gd);
    }
    v->free_blocks = fb;
    v->free_inodes = (uint32_t)fi;
}

// ---- blocuri ----

int balloc(Volume* v, uint64_t goal, uint32_t want, uint64_t& first, uint32_t& got){
    if(want == 0){
        want = 1;
    }
    uint32_t g0 = 0;
    uint32_t goalbit = 0;
    if(goal >= v->first_data_block && goal < v->blocks){
        g0 = (uint32_t)((goal - v->first_data_block) / v->bpg);
        goalbit = (uint32_t)((goal - v->first_data_block) % v->bpg);
    }
    for(uint32_t k = 0; k <= v->groups; k++){
        uint32_t g = (g0 + k) % v->groups;
        uint32_t startbit = k == 0 ? goalbit : 0;
        uint8_t gdc[64];
        int err = gd_read(v, g, gdc);
        if(err){
            return err;
        }
        if(gd_free_blk(v, gdc) == 0){
            continue;
        }
        uint8_t* gd = gd_rw(v, g);
        if(!gd){
            return -ENOSPC;
        }
        uint8_t* bm = bbitmap_rw(v, g, gd);
        if(!bm){
            return -ENOSPC;
        }
        uint64_t gstart = v->first_data_block + (uint64_t)g * v->bpg;
        uint32_t nbits = (uint32_t)min64(v->bpg, v->blocks - gstart);
        const uint8_t* fb = v->freed_bm ? v->freed_bm[g] : nullptr;
        uint32_t bit = startbit;
        while(bit < nbits && (bit_test(bm, bit) || (fb && bit_test(fb, bit)))){
            bit++;
        }
        if(bit >= nbits){
            continue;
        }
        uint32_t n = 1;
        while(n < want && bit + n < nbits && !bit_test(bm, bit + n) && !(fb && bit_test(fb, bit + n))){
            n++;
        }
        for(uint32_t i = 0; i < n; i++){
            bit_set(bm, bit + i);
        }
        gd_put32(v, gd, 0x0C, 0x2C, gd_free_blk(v, gd) - n);
        gd_set_csums(v, g, gd, bm, nullptr);
        v->free_blocks -= n;
        v->sb_dirty = true;
        first = gstart + bit;
        got = n;
        return 0;
    }
    return -ENOSPC;
}

int bfree(Volume* v, uint64_t blk, uint64_t cnt, bool metadata){
    if(blk < v->first_data_block || blk + cnt > v->blocks){
        kprintf("ext4: eliberare in afara fs (%lu +%lu)\n", blk, cnt);
        return -EIO;
    }
    while(cnt){
        uint32_t g = (uint32_t)((blk - v->first_data_block) / v->bpg);
        uint64_t gstart = v->first_data_block + (uint64_t)g * v->bpg;
        uint32_t bit = (uint32_t)(blk - gstart);
        uint32_t n = (uint32_t)min64(cnt, v->bpg - bit);
        uint8_t* gd = gd_rw(v, g);
        if(!gd){
            return -ENOSPC;
        }
        if(has_gd_csum(v) && (le16(gd + 0x12) & BG_BLOCK_UNINIT)){
            kprintf("ext4: eliberare intr-un grup BLOCK_UNINIT (%u)\n", g);
            return -EIO;
        }
        uint8_t* bm = tx_get(v, gd_bb(v, gd), true);
        if(!bm){
            return -ENOSPC;
        }
        int err = freed_snapshot(v, g, bm);
        if(err){
            return err;
        }
        uint32_t freed = 0;
        for(uint32_t i = 0; i < n; i++){
            if(bit_test(bm, bit + i)){
                bit_clear(bm, bit + i);
                freed++;
            } else {
                kprintf("ext4: blocul %lu eliberat de doua ori\n", blk + i);
            }
            if(metadata){
                tx_forget(v, blk + i, true);
            }
        }
        gd_put32(v, gd, 0x0C, 0x2C, gd_free_blk(v, gd) + freed);
        gd_set_csums(v, g, gd, bm, nullptr);
        v->free_blocks += freed;
        v->sb_dirty = true;
        blk += n;
        cnt -= n;
    }
    return 0;
}

// ---- inode-uri ----

int ialloc(Volume* v, uint32_t near_group, bool isdir, uint32_t& ino){
    for(uint32_t k = 0; k < v->groups; k++){
        uint32_t g = (near_group + k) % v->groups;
        uint8_t gdc[64];
        int err = gd_read(v, g, gdc);
        if(err){
            return err;
        }
        if(gd_free_ino(v, gdc) == 0){
            continue;
        }
        uint8_t* gd = gd_rw(v, g);
        if(!gd){
            return -ENOSPC;
        }
        bool was_uninit;
        uint8_t* bm = ibitmap_rw(v, gd, was_uninit);
        if(!bm){
            return -ENOSPC;
        }
        if(was_uninit){
            memset(bm, 0, v->bs);
            for(uint32_t i = v->ipg; i < v->bs * 8; i++){
                bit_set(bm, i);                 // marginea bitmap-ului
            }
        }
        uint32_t bit = 0;
        while(bit < v->ipg && bit_test(bm, bit)){
            bit++;
        }
        if(bit >= v->ipg){
            continue;
        }
        bit_set(bm, bit);
        gd_put32(v, gd, 0x0E, 0x2E, gd_free_ino(v, gd) - 1);
        if(isdir){
            gd_put32(v, gd, 0x10, 0x30, gd_dirs(v, gd) + 1);
        }
        if(was_uninit){
            put16(gd + 0x12, le16(gd + 0x12) & ~BG_INODE_UNINIT);
        }
        if(has_gd_csum(v) && v->ipg - gd_unused(v, gd) <= bit){
            gd_put32(v, gd, 0x1C, 0x32, v->ipg - bit - 1);
        }
        gd_set_csums(v, g, gd, nullptr, bm);
        v->free_inodes--;
        v->sb_dirty = true;
        ino = g * v->ipg + bit + 1;
        return 0;
    }
    return -ENOSPC;
}

int ifree(Volume* v, uint32_t ino, bool isdir){
    uint32_t g = (ino - 1) / v->ipg;
    uint32_t bit = (ino - 1) % v->ipg;
    uint8_t* gd = gd_rw(v, g);
    if(!gd){
        return -ENOSPC;
    }
    uint8_t* bm = tx_get(v, gd_ib(v, gd), true);
    if(!bm){
        return -ENOSPC;
    }
    if(!bit_test(bm, bit)){
        kprintf("ext4: inode-ul %u eliberat de doua ori\n", ino);
        return -EIO;
    }
    bit_clear(bm, bit);
    gd_put32(v, gd, 0x0E, 0x2E, gd_free_ino(v, gd) + 1);
    if(isdir && gd_dirs(v, gd)){
        gd_put32(v, gd, 0x10, 0x30, gd_dirs(v, gd) - 1);
    }
    gd_set_csums(v, g, gd, nullptr, bm);
    v->free_inodes++;
    v->sb_dirty = true;
    return 0;
}

}
