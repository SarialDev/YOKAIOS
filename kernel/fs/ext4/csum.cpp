#include "fs/ext4/ext4_priv.h"
#include "lib/crc.h"
#include "lib/string.h"

namespace ext4 {

uint32_t csum_inode_seed(Volume* v, uint32_t ino, uint32_t gen){
    uint8_t b[4];
    put32(b, ino);
    uint32_t c = crc32c(v->csum_seed, b, 4);
    put32(b, gen);
    return crc32c(c, b, 4);
}

void gd_set_csums(Volume* v, uint32_t g, uint8_t* gd, const uint8_t* bbitmap, const uint8_t* ibitmap){
    if(v->csum){
        if(bbitmap){
            uint32_t c = crc32c(v->csum_seed, bbitmap, v->bpg / 8);
            put16(gd + 0x18, c & 0xFFFF);
            if(v->desc_size >= 64){
                put16(gd + 0x38, c >> 16);
            }
        }
        if(ibitmap){
            uint32_t c = crc32c(v->csum_seed, ibitmap, v->ipg / 8);
            put16(gd + 0x1A, c & 0xFFFF);
            if(v->desc_size >= 64){
                put16(gd + 0x3A, c >> 16);
            }
        }
    }
    gd_csum_update(v, g, gd);
}

void gd_csum_update(Volume* v, uint32_t g, uint8_t* gd){
    if(!v->csum && !v->gdt_csum){
        return;
    }
    uint8_t gb[4];
    put32(gb, g);
    put16(gd + 0x1E, 0);
    if(v->csum){
        uint32_t c = crc32c(v->csum_seed, gb, 4);
        c = crc32c(c, gd, v->desc_size);
        put16(gd + 0x1E, c & 0xFFFF);
    } else {
        uint16_t c = crc16(0xFFFF, v->uuid, 16);
        c = crc16(c, gb, 4);
        c = crc16(c, gd, 0x1E);
        if(v->desc_size > 0x20){
            c = crc16(c, gd + 0x20, v->desc_size - 0x20);
        }
        put16(gd + 0x1E, c);
    }
}

void sb_csum_update(Volume* v, uint8_t* sb){
    if(v->csum){
        put32(sb + 0x3FC, crc32c(0xFFFFFFFFu, sb, 0x3FC));
    }
}

void inode_csum_set(Volume* v, uint32_t ino, uint8_t* raw){
    if(!v->csum){
        return;
    }
    uint32_t extra = v->inode_size > 128 ? le16(raw + 128) : 0;
    bool hi = v->inode_size > 128 && extra >= 4;
    put16(raw + 0x7C, 0);
    if(hi){
        put16(raw + 0x82, 0);
    }
    uint32_t c = crc32c(csum_inode_seed(v, ino, le32(raw + 100)), raw, v->inode_size);
    put16(raw + 0x7C, c & 0xFFFF);
    if(hi){
        put16(raw + 0x82, c >> 16);
    }
}

void dirblock_csum_set(Node* d, uint8_t* blk){
    Volume* v = d->vol;
    if(!v->csum){
        return;
    }
    uint8_t* t = blk + v->bs - 12;
    if(le32(t) != 0 || le16(t + 4) != 12 || t[7] != 0xDE){
        return;
    }
    put32(t + 8, crc32c(csum_inode_seed(v, (uint32_t)d->ino->ino, d->gen), blk, v->bs - 12));
}

void dxnode_csum_set(Node* d, uint8_t* blk, uint32_t count_off){
    Volume* v = d->vol;
    if(!v->csum){
        return;
    }
    uint32_t limit = le16(blk + count_off);
    uint32_t count = le16(blk + count_off + 2);
    uint32_t toff = count_off + limit * 8;
    if(toff + 8 > v->bs || count > limit){
        return;
    }
    uint32_t c = crc32c(csum_inode_seed(v, (uint32_t)d->ino->ino, d->gen), blk, count_off + count * 8);
    c = crc32c(c, blk + toff, 4);
    uint8_t zero[4] = {0, 0, 0, 0};
    c = crc32c(c, zero, 4);
    put32(blk + toff + 4, c);
}

void extblock_csum_set(Node* n, uint8_t* blk){
    Volume* v = n->vol;
    if(!v->csum){
        return;
    }
    uint32_t max = le16(blk + 4);
    uint32_t toff = 12 + 12 * max;
    if(toff + 4 > v->bs){
        return;
    }
    put32(blk + toff, crc32c(csum_inode_seed(v, (uint32_t)n->ino->ino, n->gen), blk, toff));
}

}
