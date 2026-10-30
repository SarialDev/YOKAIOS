#include "fs/ext4/ext4_priv.h"
#include "lib/string.h"
#include "lib/new.h"
#include "mm/kmalloc.h"
#include "drivers/rtc.h"
#include "time/clock.h"
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


namespace {

Timespec ts_of(uint32_t secs, uint32_t extra, bool has_extra){
    Timespec t;
    t.sec = (int32_t)secs;
    t.nsec = 0;
    if(has_extra){
        t.sec += (int64_t)(extra & 3) << 32;
        t.nsec = extra >> 2;
    }
    return t;
}

void ts_put(uint8_t* raw, uint32_t lo_off, uint32_t extra_off, bool has_extra, const Timespec& t){
    put32(raw + lo_off, (uint32_t)t.sec);
    if(has_extra){
        put32(raw + extra_off, (uint32_t)(((t.sec - (int32_t)t.sec) >> 32) & 3) | (uint32_t)t.nsec << 2);
    }
}

}

uint64_t inode_offset(Volume* v, uint32_t ino){
    uint32_t g = (ino - 1) / v->ipg;
    uint32_t idx = (ino - 1) % v->ipg;
    return (v->itable[g] << v->bs_shift) + (uint64_t)idx * v->inode_size;
}

// Inode-ul ino (incarcat o singura data; ramane in memorie). Cu lock-ul volumului luat
int node_load(Volume* v, uint32_t ino, Node*& out, bool allow_dead){
    for(Node* n = v->hash[ino % NODE_BUCKETS]; n; n = n->next){
        if(n->ino->ino == ino){
            out = n;
            return 0;
        }
    }
    if(ino == 0 || ino > v->inodes){
        return -EIO;
    }
    uint32_t g = (ino - 1) / v->ipg;
    if(g >= v->groups){
        return -EIO;
    }
    uint8_t raw[160];
    uint32_t rl = v->inode_size < sizeof(raw) ? v->inode_size : sizeof(raw);
    int err = read_bytes(v, inode_offset(v, ino), raw, rl);
    if(err){
        return err;
    }
    uint32_t mode = le16(raw);
    uint32_t type = mode & S_IFMT;
    if(type != S_IFREG && type != S_IFDIR && type != S_IFLNK && type != S_IFCHR && type != S_IFBLK &&
       type != S_IFIFO && type != S_IFSOCK_){
        return -EIO;
    }
    uint32_t links = le16(raw + 26);
    uint32_t flags = le32(raw + 32);
    if((links == 0 && !allow_dead) || (flags & INODE_INLINE_FL)){
        return links == 0 ? -ENOENT : -EIO;
    }
    uint32_t extra = v->inode_size > 128 ? le16(raw + 128) : 0;
    if(extra > v->inode_size - 128){
        extra = 0;
    }
    auto fits = [&](uint32_t field){ return field + 4 <= 128 + extra && field + 4 <= rl; };

    void* mem = kzalloc(sizeof(Node));
    if(!mem){
        return -ENOMEM;
    }
    Node* n = (Node*)mem;
    Inode* i = inode_alloc(v->sb, ino, mode, v->iops);
    if(!i){
        kfree(n);
        return -ENOMEM;
    }
    i->priv = n;
    i->uid = le16(raw + 2) | (uint32_t)le16(raw + 120) << 16;
    i->gid = le16(raw + 24) | (uint32_t)le16(raw + 122) << 16;
    i->nlink = links;
    i->size = le32(raw + 4);
    if(type == S_IFREG){
        i->size |= (uint64_t)le32(raw + 108) << 32;
    }
    i->atime = ts_of(le32(raw + 8), fits(140) ? le32(raw + 140) : 0, fits(140));
    i->ctime = ts_of(le32(raw + 12), fits(132) ? le32(raw + 132) : 0, fits(132));
    i->mtime = ts_of(le32(raw + 16), fits(136) ? le32(raw + 136) : 0, fits(136));
    n->vol = v;
    n->ino = i;
    n->flags = flags;
    n->extra_isize = extra;
    n->gen = le32(raw + 100);
    n->dtime = le32(raw + 20);
    n->file_acl = le32(raw + 104);
    memcpy(n->blk, raw + 40, 60);
    n->blocks512 = le32(raw + 28) | (uint64_t)le16(raw + 116) << 32;
    uint32_t ea = n->file_acl ? v->bs >> 9 : 0;
    n->fast_symlink = type == S_IFLNK && n->blocks512 == ea;
    if(n->fast_symlink && i->size > 60){
        inode_free(i);
        kfree(n);
        return -EIO;
    }
    n->next = v->hash[ino % NODE_BUCKETS];
    v->hash[ino % NODE_BUCKETS] = n;
    out = n;
    return 0;
}

int inode_store(Node* n){
    Volume* v = n->vol;
    Inode* i = n->ino;
    uint64_t off = inode_offset(v, (uint32_t)i->ino);
    uint8_t* blk = tx_get(v, off >> v->bs_shift, true);
    if(!blk){
        return -ENOSPC;
    }
    uint8_t* raw = blk + (off & (v->bs - 1));
    uint32_t extra = n->extra_isize;
    auto fits = [&](uint32_t field){ return field + 4 <= 128 + extra; };
    put16(raw, i->mode);
    put16(raw + 2, i->uid);
    put16(raw + 120, i->uid >> 16);
    put16(raw + 24, i->gid);
    put16(raw + 122, i->gid >> 16);
    put16(raw + 26, i->nlink);
    put32(raw + 4, (uint32_t)i->size);
    if(S_ISREG(i->mode)){
        put32(raw + 108, (uint32_t)(i->size >> 32));
    }
    ts_put(raw, 8, 140, fits(140), i->atime);
    ts_put(raw, 12, 132, fits(132), i->ctime);
    ts_put(raw, 16, 136, fits(136), i->mtime);
    put32(raw + 20, n->dtime);
    put32(raw + 28, (uint32_t)n->blocks512);
    put16(raw + 116, (uint32_t)(n->blocks512 >> 32));
    put32(raw + 32, n->flags);
    put32(raw + 104, n->file_acl);
    memcpy(raw + 40, n->blk, 60);
    inode_csum_set(v, (uint32_t)i->ino, raw);
    return 0;
}

int inode_init(Volume* v, uint32_t ino, uint32_t mode, uint32_t flags, Node*& out){
    uint64_t off = inode_offset(v, ino);
    uint8_t* blk = tx_get(v, off >> v->bs_shift, true);
    if(!blk){
        return -ENOSPC;
    }
    uint8_t* raw = blk + (off & (v->bs - 1));
    memset(raw, 0, v->inode_size);
    Timespec now = vfs_now();
    uint32_t extra = v->inode_size > 128 ? (v->inode_size - 128 < 32 ? v->inode_size - 128 : 32) : 0;
    put16(raw, mode);
    put16(raw + 26, 1);
    put32(raw + 32, flags);
    put32(raw + 100, ++v->gen_ctr * 2654435761u ^ (uint32_t)uptime_ms());
    if(extra){
        put16(raw + 128, extra);
    }
    auto fits = [&](uint32_t field){ return field + 4 <= 128 + extra; };
    ts_put(raw, 8, 140, fits(140), now);
    ts_put(raw, 12, 132, fits(132), now);
    ts_put(raw, 16, 136, fits(136), now);
    ts_put(raw, 144, 148, fits(148), now);        // crtime
    if(flags & INODE_EXTENTS_FL){
        uint32_t root[15] = {};
        ext_init_root(root);
        memcpy(raw + 40, root, 60);
    }
    inode_csum_set(v, ino, raw);
    return node_load(v, ino, out);
}

}
