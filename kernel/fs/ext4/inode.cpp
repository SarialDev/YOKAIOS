#include "fs/ext4/ext4_priv.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace ext4 {

namespace {

const uint16_t EXT_MAGIC = 0xF30A;
const uint32_t EXT_MAX_DEPTH = 5;

uint64_t min64(uint64_t a, uint64_t b){
    return a < b ? a : b;
}

uint64_t pw(uint64_t base, int e){
    uint64_t r = 1;
    while(e-- > 0){
        r *= base;
    }
    return r;
}

// Arborele de extente: radacina in i_block (60 octeti), nodurile interioare / frunzele in blocuri fs.
// limit = primul bloc logic care nu mai apartine subarborelui curent (marginea golurilor)
int extent_map(Node* n, uint64_t lblk, uint64_t maxrun, uint64_t& phys, uint64_t& run){
    Volume* v = n->vol;
    const uint8_t* p = (const uint8_t*)n->blk;
    uint32_t avail = 60;
    Buf* held = nullptr;
    uint64_t limit = ~0ull;
    int rc = -EIO;
    for(uint32_t level = 0; level <= EXT_MAX_DEPTH; level++){
        if(le16(p) != EXT_MAGIC){
            break;
        }
        uint32_t entries = le16(p + 2);
        uint32_t depth = le16(p + 6);
        if(depth > EXT_MAX_DEPTH - level || entries > (avail - 12) / 12){
            break;
        }
        const uint8_t* e = p + 12;
        uint32_t i = 0;
        int found = -1;
        while(i < entries && le32(e + 12 * i) <= lblk){
            found = (int)i;
            i++;
        }
        uint64_t next = i < entries ? le32(e + 12 * i) : limit;
        if(next > limit){
            next = limit;
        }
        if(depth == 0 || found < 0){
            // Frunza (sau inainte de primul subarbore): gaura pana la urmatorul extent, daca lblk nu e acoperit
            phys = 0;
            run = min64(next - lblk, maxrun);
            rc = 0;
            if(depth == 0 && found >= 0){
                const uint8_t* x = e + 12 * found;
                uint64_t start = le32(x);
                uint32_t len = le16(x + 4);
                bool uninit = false;
                if(len > 32768){
                    len -= 32768;
                    uninit = true;
                }
                if(lblk < start + len){
                    run = min64(start + len - lblk, maxrun);
                    if(!uninit){
                        uint64_t first = ((uint64_t)le16(x + 6) << 32 | le32(x + 8)) + (lblk - start);
                        if(first + run > v->blocks){
                            rc = -EIO;
                        } else {
                            phys = first;
                        }
                    }
                }
            }
            break;
        }
        const uint8_t* x = e + 12 * found;
        uint64_t child = (uint64_t)le16(x + 8) << 32 | le32(x + 4);
        limit = next;
        Buf* nb;
        const uint8_t* np = fsblk_get(v, child, nb);
        if(held){
            brelse(held);
        }
        held = nb;
        if(!np){
            held = nullptr;
            break;
        }
        p = np;
        avail = v->bs;
    }
    if(held){
        brelse(held);
    }
    return rc;
}

// Harta de blocuri ext2/ext3: 12 directe, apoi indirect simplu, dublu, triplu (pointeri pe 32 biti)
int bmap_map(Node* n, uint64_t lblk, uint64_t maxrun, uint64_t& phys, uint64_t& run){
    Volume* v = n->vol;
    uint64_t ppb = v->bs / 4;
    if(lblk < 12){
        uint32_t e = n->blk[lblk];
        run = 1;
        while(lblk + run < 12 && run < maxrun && (e ? n->blk[lblk + run] == e + run : n->blk[lblk + run] == 0)){
            run++;
        }
        phys = e;
        return e >= v->blocks ? -EIO : 0;
    }
    uint64_t l = lblk - 12;
    int depth;
    uint32_t top;
    if(l < ppb){
        depth = 1;
        top = n->blk[12];
    } else if((l -= ppb) < ppb * ppb){
        depth = 2;
        top = n->blk[13];
    } else if((l -= ppb * ppb) < ppb * ppb * ppb){
        depth = 3;
        top = n->blk[14];
    } else {
        phys = 0;                               // dincolo de ce poate adresa harta
        run = maxrun;
        return 0;
    }
    uint32_t blk = top;
    for(int level = depth; level > 0; level--){
        if(blk == 0){
            uint64_t cov = pw(ppb, level);      // gaura acopera tot subarborele de la acest nivel
            phys = 0;
            run = min64(cov - l % cov, maxrun);
            return 0;
        }
        Buf* b;
        const uint8_t* t = fsblk_get(v, blk, b);
        if(!t){
            return -EIO;
        }
        uint64_t idx = (l / pw(ppb, level - 1)) % ppb;
        uint32_t e = le32(t + idx * 4);
        if(level == 1){
            run = 1;
            if(e == 0){
                while(idx + run < ppb && run < maxrun && le32(t + (idx + run) * 4) == 0){
                    run++;
                }
            } else {
                while(idx + run < ppb && run < maxrun && le32(t + (idx + run) * 4) == e + run){
                    run++;
                }
            }
            phys = e;
            brelse(b);
            return e >= v->blocks ? -EIO : 0;
        }
        brelse(b);
        blk = e;
    }
    return -EIO;
}

}

uint8_t* fsblk_get(Volume* v, uint64_t blk, Buf*& b){
    b = nullptr;
    if(blk >= v->blocks){
        return nullptr;
    }
    uint64_t off = blk << v->bs_shift;
    b = bread(v->dev, off / BCACHE_BLOCK);
    if(!b){
        return nullptr;
    }
    return b->data + off % BCACHE_BLOCK;
}

int read_bytes(Volume* v, uint64_t off, void* buf, uint64_t len){
    uint8_t* out = (uint8_t*)buf;
    while(len){
        uint32_t bo = (uint32_t)(off % BCACHE_BLOCK);
        uint64_t n = min64(len, BCACHE_BLOCK - bo);
        Buf* b = bread(v->dev, off / BCACHE_BLOCK);
        if(!b){
            return -EIO;
        }
        memcpy(out, b->data + bo, n);
        brelse(b);
        off += n;
        out += n;
        len -= n;
    }
    return 0;
}

int map_block(Node* n, uint64_t lblk, uint64_t maxrun, uint64_t& phys, uint64_t& run){
    if(maxrun == 0){
        maxrun = 1;
    }
    if(n->flags & INODE_EXTENTS_FL){
        return extent_map(n, lblk, maxrun, phys, run);
    }
    return bmap_map(n, lblk, maxrun, phys, run);
}

int64_t node_read(Node* n, uint64_t pos, void* buf, uint64_t len){
    Volume* v = n->vol;
    uint64_t size = n->ino->size;
    if(pos >= size || len == 0){
        return 0;
    }
    if(len > size - pos){
        len = size - pos;
    }
    uint8_t* out = (uint8_t*)buf;
    uint64_t last = (size - 1) >> v->bs_shift;
    uint64_t done = 0;
    while(done < len){
        uint64_t p = pos + done;
        uint64_t lblk = p >> v->bs_shift;
        uint64_t boff = p & (v->bs - 1);
        uint64_t phys, run;
        int err = map_block(n, lblk, last - lblk + 1, phys, run);
        if(err){
            return done ? (int64_t)done : err;
        }
        uint64_t chunk = min64(run * v->bs - boff, len - done);
        if(phys){
            err = read_bytes(v, (phys << v->bs_shift) + boff, out + done, chunk);
            if(err){
                return done ? (int64_t)done : err;
            }
        } else {
            memset(out + done, 0, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

}
