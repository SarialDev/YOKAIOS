#include "fs/ext4/ext4_priv.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

// Modificarea directoarelor (T6.8): intrari adaugate / scoase / schimbate in directoare liniare si cu index htree
// (la un director indexat, frunza plina se sparge in doua si indexul primeste o intrare; daca nodul dx cel mai de jos
// e plin intoarce -ENOSPC). Directoarele liniare cresc cu blocuri noi.

namespace ext4 {

namespace {

uint32_t rec_need(uint32_t len){
    return (8 + len + 3) & ~3u;
}

uint32_t usable_end(Volume* v){
    return v->csum ? v->bs - 12 : v->bs;
}

bool is_tail(Volume* v, const uint8_t* blk, uint32_t off){
    return off + 12 == v->bs && le32(blk + off) == 0 && le16(blk + off + 4) == 12 && blk[off + 7] == 0xDE;
}

bool is_dxnode(Volume* v, const uint8_t* blk){
    return le32(blk) == 0 && le16(blk + 4) == v->bs;
}

uint32_t dir_nblocks(Node* d){
    return (uint32_t)(d->ino->size >> d->vol->bs_shift);
}

void leaf_init(Volume* v, uint8_t* blk){
    memset(blk, 0, v->bs);
    put16(blk + 4, usable_end(v));
    if(v->csum){
        uint8_t* t = blk + v->bs - 12;
        put16(t + 4, 12);
        t[7] = 0xDE;
    }
}

void blk_fixup(Node* d, uint8_t* blk, uint32_t lblk){
    Volume* v = d->vol;
    if(d->flags & INODE_INDEX_FL){
        if(lblk == 0){
            dxnode_csum_set(d, blk, 32);
            return;
        }
        if(is_dxnode(v, blk)){
            dxnode_csum_set(d, blk, 8);
            return;
        }
    }
    dirblock_csum_set(d, blk);
}

// Blocul fizic al blocului logic lblk (0 = gaura)
int phys_of(Node* d, uint32_t lblk, uint64_t& phys){
    uint64_t run;
    int rc = map_block(d, lblk, 1, phys, run);
    if(rc){
        return rc;
    }
    return phys ? 0 : -EIO;
}

bool room_in(Volume* v, const uint8_t* blk, uint32_t need){
    for(uint32_t off = 0; off < v->bs;){
        if(is_tail(v, blk, off)){
            break;
        }
        uint32_t rec = le16(blk + off + 4);
        if(rec < 8 || (rec & 3) || off + rec > v->bs){
            return false;
        }
        uint32_t used = le32(blk + off) ? rec_need(blk[off + 6]) : 0;
        if(rec >= used + need){
            return true;
        }
        off += rec;
    }
    return false;
}

bool insert_in(Volume* v, uint8_t* blk, const char* name, uint32_t len, uint32_t ino, uint8_t ftype){
    uint32_t need = rec_need(len);
    for(uint32_t off = 0; off < v->bs;){
        if(is_tail(v, blk, off)){
            break;
        }
        uint32_t rec = le16(blk + off + 4);
        if(rec < 8 || (rec & 3) || off + rec > v->bs){
            return false;
        }
        uint32_t used = le32(blk + off) ? rec_need(blk[off + 6]) : 0;
        if(rec >= used + need){
            uint32_t at = off;
            uint32_t newrec = rec;
            if(used){
                put16(blk + off + 4, used);
                at = off + used;
                newrec = rec - used;
            }
            put32(blk + at, ino);
            put16(blk + at + 4, newrec);
            blk[at + 6] = (uint8_t)len;
            blk[at + 7] = ftype;
            memcpy(blk + at + 8, name, len);
            return true;
        }
        off += rec;
    }
    return false;
}

// ---- htree ----

struct Frame {
    uint32_t lblk;
    uint32_t off;                               // inceputul count/limit in bloc
    uint32_t count;
    uint32_t limit;
    uint32_t pos;
};

// 0 gasit; 1 = structura neasteptata; <0 eroare
int dx_find(Node* d, const char* name, uint32_t len, Frame* fr, uint32_t& levels, uint32_t& leaf, uint32_t& hash,
            uint32_t& ver){
    Volume* v = d->vol;
    Buf* b;
    uint8_t* p = dir_block(d, 0, b);
    if(!p){
        return -EIO;
    }
    bool ok = le16(p + 4) == 12 && le16(p + 16) == v->bs - 12 && p[24 + 5] == 8 && le32(p + 24) == 0;
    ver = p[24 + 4];
    levels = p[24 + 6];
    brelse(b);
    if(!ok || levels >= 4 || ver > 5 || !dir_name_hash(v, ver, name, len, hash)){
        return 1;
    }
    uint32_t nblocks = dir_nblocks(d);
    uint32_t lblk = 0;
    uint32_t off = 32;
    for(uint32_t l = 0;; l++){
        p = dir_block(d, lblk, b);
        if(!p){
            return -EIO;
        }
        uint32_t limit = le16(p + off);
        uint32_t count = le16(p + off + 2);
        if(count == 0 || count > limit || off + limit * 8 > v->bs || (off == 8 && !is_dxnode(v, p))){
            brelse(b);
            return 1;
        }
        uint32_t pos = 0;
        for(uint32_t i = 1; i < count && le32(p + off + 8 * i) <= hash; i++){
            pos = i;
        }
        uint32_t child = le32(p + off + 8 * pos + 4) & 0x0fffffffu;
        brelse(b);
        fr[l] = Frame{lblk, off, count, limit, pos};
        if(child >= nblocks){
            return -EIO;
        }
        if(l == levels){
            leaf = child;
            return 0;
        }
        lblk = child;
        off = 8;
    }
}

struct EI {
    uint32_t hash;
    uint32_t off;
};

// Scrie intrarile e[from..to) ale blocului src, in ordine, compact in dst (initializat deja)
void pack(Volume* v, uint8_t* dst, const uint8_t* src, const EI* e, uint32_t from, uint32_t to){
    uint32_t end = usable_end(v);
    uint32_t off = 0;
    for(uint32_t i = from; i < to; i++){
        uint32_t nlen = src[e[i].off + 6];
        uint32_t rec = i == to - 1 ? end - off : rec_need(nlen);
        memcpy(dst + off, src + e[i].off, 8 + nlen);
        put16(dst + off + 4, rec);
        off += rec;
    }
}

// Aloca un bloc nou la sfarsitul directorului (zero), intoarce pointerul in tranzactie
int dir_grow(Node* d, uint32_t& lblk, uint8_t*& blk){
    Volume* v = d->vol;
    lblk = dir_nblocks(d);
    uint64_t first;
    uint32_t got;
    int rc = file_alloc_range(d, lblk, 1, first, got);
    if(rc){
        return rc;
    }
    blk = tx_get(v, first, false);
    if(!blk){
        return -ENOSPC;
    }
    d->ino->size += v->bs;
    return inode_store(d);
}

int split_and_add(Node* d, Frame* fr, uint32_t levels, uint32_t leaf, uint32_t ver, const char* name, uint32_t len,
                  uint32_t ino, uint8_t ftype, uint32_t newhash){
    Volume* v = d->vol;
    Frame& lf = fr[levels];
    if(lf.count >= lf.limit){
        return -ENOSPC;                         // nodul dx cel mai de jos e plin (nu se sparge)
    }
    uint64_t lphys, dphys;
    int rc = phys_of(d, leaf, lphys);
    if(rc){
        return rc;
    }
    rc = phys_of(d, lf.lblk, dphys);
    if(rc){
        return rc;
    }
    uint8_t* orig = tx_get(v, lphys, true);
    uint8_t* copy = (uint8_t*)kmalloc(v->bs);
    EI* e = (EI*)kmalloc(sizeof(EI) * (v->bs / 12 + 1));
    if(!orig || !copy || !e){
        kfree(copy);
        kfree(e);
        return orig ? -ENOMEM : -ENOSPC;
    }
    memcpy(copy, orig, v->bs);
    uint32_t n = 0;
    uint32_t total = 0;
    for(uint32_t off = 0; off < v->bs;){
        if(is_tail(v, copy, off)){
            break;
        }
        uint32_t rec = le16(copy + off + 4);
        if(rec < 8 || (rec & 3) || off + rec > v->bs){
            rc = -EIO;
            break;
        }
        if(le32(copy + off)){
            uint32_t nl = copy[off + 6];
            uint32_t h;
            if(!dir_name_hash(v, ver, (const char*)copy + off + 8, nl, h)){
                rc = -EIO;
                break;
            }
            e[n++] = EI{h, off};
            total += rec_need(nl);
        }
        off += rec;
    }
    if(!rc && n < 2){
        rc = -ENOSPC;
    }
    if(rc){
        kfree(copy);
        kfree(e);
        return rc;
    }
    for(uint32_t i = 1; i < n; i++){            // sortare dupa hash (insertie)
        EI x = e[i];
        uint32_t j = i;
        while(j > 0 && e[j - 1].hash > x.hash){
            e[j] = e[j - 1];
            j--;
        }
        e[j] = x;
    }
    uint32_t mid = 0;
    uint32_t acc = 0;
    for(uint32_t i = 0; i < n; i++){
        acc += rec_need(copy[e[i].off + 6]);
        if(acc >= total / 2){
            mid = i + 1;
            break;
        }
    }
    if(mid < 1){
        mid = 1;
    }
    if(mid > n - 1){
        mid = n - 1;
    }
    uint32_t key = e[mid].hash;
    bool cont = e[mid].hash == e[mid - 1].hash;
    uint32_t nl;
    uint8_t* nb;
    rc = dir_grow(d, nl, nb);
    if(rc){
        kfree(copy);
        kfree(e);
        return rc;
    }
    leaf_init(v, nb);
    pack(v, nb, copy, e, mid, n);
    leaf_init(v, orig);
    pack(v, orig, copy, e, 0, mid);
    blk_fixup(d, nb, nl);
    blk_fixup(d, orig, leaf);
    kfree(copy);
    kfree(e);
    // intrarea noua in nodul dx
    uint8_t* dx = tx_get(v, dphys, true);
    if(!dx){
        return -ENOSPC;
    }
    uint8_t* arr = dx + lf.off;
    uint32_t at = lf.pos + 1;
    memmove(arr + 8 * (at + 1), arr + 8 * at, (size_t)(lf.count - at) * 8);
    put32(arr + 8 * at, key | (cont ? 1u : 0u));
    put32(arr + 8 * at + 4, nl);
    put16(arr + 2, lf.count + 1);
    blk_fixup(d, dx, lf.lblk);
    // intrarea ceruta, in jumatatea potrivita
    bool upper = newhash >= key;
    uint8_t* target = upper ? nb : orig;
    if(!insert_in(v, target, name, len, ino, ftype)){
        return -ENOSPC;
    }
    blk_fixup(d, target, upper ? nl : leaf);
    return 0;
}

int add_indexed(Node* d, const char* name, uint32_t len, uint32_t ino, uint8_t ftype){
    Volume* v = d->vol;
    Frame fr[4];
    uint32_t levels, leaf, hash, ver;
    int rc = dx_find(d, name, len, fr, levels, leaf, hash, ver);
    if(rc > 0){
        return -EIO;
    }
    if(rc){
        return rc;
    }
    uint64_t phys;
    rc = phys_of(d, leaf, phys);
    if(rc){
        return rc;
    }
    Buf* b;
    uint8_t* rp = fsblk_get(v, phys, b);
    if(!rp){
        return -EIO;
    }
    bool room = room_in(v, rp, rec_need(len));
    brelse(b);
    if(room){
        uint8_t* wp = tx_get(v, phys, true);
        if(!wp){
            return -ENOSPC;
        }
        if(!insert_in(v, wp, name, len, ino, ftype)){
            return -EIO;
        }
        blk_fixup(d, wp, leaf);
        return 0;
    }
    return split_and_add(d, fr, levels, leaf, ver, name, len, ino, ftype, hash);
}

// Cauta numele (scanare liniara a tuturor blocurilor, inclusiv frunzele unui index). 0 gasit
int find_entry(Node* d, const char* name, uint32_t len, uint32_t& lblk, uint32_t& off, uint32_t& prev){
    Volume* v = d->vol;
    uint32_t n = dir_nblocks(d);
    for(uint32_t l = 0; l < n; l++){
        Buf* b;
        uint8_t* blk = dir_block(d, l, b);
        if(!blk){
            return -EIO;
        }
        int rc = -ENOENT;
        uint32_t pv = 0;
        for(uint32_t o = 0; o < v->bs;){
            uint32_t rec = le16(blk + o + 4);
            if(rec < 8 || (rec & 3) || o + rec > v->bs){
                rc = -EIO;
                break;
            }
            if(le32(blk + o) && blk[o + 6] == len && memcmp(blk + o + 8, name, len) == 0){
                lblk = l;
                off = o;
                prev = pv;
                rc = 0;
                break;
            }
            pv = o;
            o += rec;
        }
        brelse(b);
        if(rc != -ENOENT){
            return rc;
        }
    }
    return -ENOENT;
}

}

uint8_t ftype_of_mode(uint32_t mode){
    switch(mode & S_IFMT){
    case S_IFREG: return 1;
    case S_IFDIR: return 2;
    case S_IFCHR: return 3;
    case S_IFBLK: return 4;
    case S_IFIFO: return 5;
    case S_IFSOCK_: return 6;
    case S_IFLNK: return 7;
    default: return 0;
    }
}

int dir_init_block(Node* d, uint8_t* blk, uint32_t self, uint32_t parent){
    Volume* v = d->vol;
    leaf_init(v, blk);
    put32(blk, self);
    put16(blk + 4, 12);
    blk[6] = 1;
    blk[7] = 2;
    blk[8] = '.';
    put32(blk + 12, parent);
    put16(blk + 16, usable_end(v) - 12);
    blk[18] = 2;
    blk[19] = 2;
    blk[20] = '.';
    blk[21] = '.';
    dirblock_csum_set(d, blk);
    return 0;
}

int dir_add(Node* d, const char* name, uint32_t len, uint32_t ino, uint8_t ftype){
    Volume* v = d->vol;
    d->rd_valid = false;
    if(d->flags & INODE_INDEX_FL){
        return add_indexed(d, name, len, ino, ftype);
    }
    uint32_t need = rec_need(len);
    uint32_t n = dir_nblocks(d);
    for(uint32_t l = 0; l < n; l++){
        Buf* b;
        uint8_t* blk = dir_block(d, l, b);
        if(!blk){
            return -EIO;
        }
        bool room = room_in(v, blk, need);
        brelse(b);
        if(room){
            uint64_t phys;
            int rc = phys_of(d, l, phys);
            if(rc){
                return rc;
            }
            uint8_t* wp = tx_get(v, phys, true);
            if(!wp){
                return -ENOSPC;
            }
            if(!insert_in(v, wp, name, len, ino, ftype)){
                return -EIO;
            }
            blk_fixup(d, wp, l);
            return 0;
        }
    }
    uint32_t nl;
    uint8_t* nb;
    int rc = dir_grow(d, nl, nb);
    if(rc){
        return rc;
    }
    leaf_init(v, nb);
    if(!insert_in(v, nb, name, len, ino, ftype)){
        return -EIO;
    }
    blk_fixup(d, nb, nl);
    return 0;
}

int dir_remove(Node* d, const char* name, uint32_t len){
    Volume* v = d->vol;
    d->rd_valid = false;
    uint32_t lblk, off, prev;
    int rc = find_entry(d, name, len, lblk, off, prev);
    if(rc){
        return rc;
    }
    uint64_t phys;
    rc = phys_of(d, lblk, phys);
    if(rc){
        return rc;
    }
    uint8_t* wp = tx_get(v, phys, true);
    if(!wp){
        return -ENOSPC;
    }
    if(off == 0){
        put32(wp, 0);
    } else {
        put16(wp + prev + 4, le16(wp + prev + 4) + le16(wp + off + 4));
    }
    blk_fixup(d, wp, lblk);
    return 0;
}

int dir_set_ino(Node* d, const char* name, uint32_t len, uint32_t ino, uint8_t ftype){
    Volume* v = d->vol;
    d->rd_valid = false;
    uint32_t lblk, off, prev;
    int rc = find_entry(d, name, len, lblk, off, prev);
    if(rc){
        return rc;
    }
    uint64_t phys;
    rc = phys_of(d, lblk, phys);
    if(rc){
        return rc;
    }
    uint8_t* wp = tx_get(v, phys, true);
    if(!wp){
        return -ENOSPC;
    }
    put32(wp + off, ino);
    wp[off + 7] = ftype;
    blk_fixup(d, wp, lblk);
    return 0;
}

int dir_set_dotdot(Node* d, uint32_t parent){
    Volume* v = d->vol;
    d->rd_valid = false;
    uint64_t phys;
    int rc = phys_of(d, 0, phys);
    if(rc){
        return rc;
    }
    uint8_t* wp = tx_get(v, phys, true);
    if(!wp){
        return -ENOSPC;
    }
    if(le16(wp + 4) != 12 || wp[12 + 6] != 2 || wp[12 + 8] != '.' || wp[12 + 9] != '.'){
        return -EIO;
    }
    put32(wp + 12, parent);
    blk_fixup(d, wp, 0);
    return 0;
}

bool dir_is_empty(Node* d, int& err){
    DirEntry e;
    d->rd_valid = false;
    int r = dir_readdir(d, 0, e);
    err = r < 0 ? r : 0;
    return r == 0;
}

}
