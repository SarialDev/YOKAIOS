#include "fs/ext4/ext4_priv.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace ext4 {

uint64_t stat_htree = 0;
uint64_t stat_linear = 0;

namespace {

// ---- intrari de director ----

struct Dirent {
    uint32_t ino;
    uint32_t rec;
    uint32_t nlen;
    uint8_t type;
    const uint8_t* name;
};

// Intrarea de la off in blocul de director (bs octeti); false daca e corupta (rec_len gresit)
bool dirent_at(const uint8_t* blk, uint32_t bs, uint32_t off, Dirent& d){
    if(off + 8 > bs){
        return false;
    }
    const uint8_t* p = blk + off;
    d.ino = le32(p);
    d.rec = le16(p + 4);
    d.nlen = p[6];
    d.type = p[7];
    d.name = p + 8;
    return d.rec >= 8 && !(d.rec & 3) && off + d.rec <= bs && 8 + d.nlen <= d.rec;
}

uint8_t dtype_of(Volume* v, uint8_t t){
    if(!(v->incompat & INCOMPAT_FILETYPE)){
        return DT_UNKNOWN;
    }
    switch(t){
    case 1: return DT_REG;
    case 2: return DT_DIR;
    case 3: return DT_CHR;
    case 4: return DT_BLK;
    case 5: return DT_FIFO;
    case 6: return 12;                          // DT_SOCK
    case 7: return DT_LNK;
    default: return DT_UNKNOWN;
    }
}

uint32_t dir_blocks(Node* d){
    Volume* v = d->vol;
    return (uint32_t)((d->ino->size + v->bs - 1) >> v->bs_shift);
}

// Blocul logic lblk al directorului, pinned in b; nullptr la gaura sau eroare de I/O
uint8_t* dir_block(Node* d, uint32_t lblk, Buf*& b){
    uint64_t phys, run;
    b = nullptr;
    if(map_block(d, lblk, 1, phys, run) || phys == 0){
        return nullptr;
    }
    return fsblk_get(d->vol, phys, b);
}

// Numele din blocul logic lblk; 0 gasit, -ENOENT, -EIO
int scan_block(Node* d, uint32_t lblk, const char* name, uint32_t len, uint32_t& ino){
    Volume* v = d->vol;
    Buf* b;
    uint8_t* blk = dir_block(d, lblk, b);
    if(!blk){
        return -EIO;
    }
    int rc = -ENOENT;
    for(uint32_t off = 0; off < v->bs;){
        Dirent e;
        if(!dirent_at(blk, v->bs, off, e)){
            rc = -EIO;
            break;
        }
        if(e.ino && e.nlen == len && memcmp(e.name, name, len) == 0){
            ino = e.ino;
            rc = 0;
            break;
        }
        off += e.rec;
    }
    brelse(b);
    return rc;
}

int linear_lookup(Node* d, const char* name, uint32_t len, uint32_t& ino){
    stat_linear++;
    uint32_t n = dir_blocks(d);
    for(uint32_t lblk = 0; lblk < n; lblk++){
        int rc = scan_block(d, lblk, name, len, ino);
        if(rc != -ENOENT){
            return rc;
        }
    }
    return -ENOENT;
}

// ---- hash-ul htree (fs/ext4/hash.c) ----

uint32_t rol32(uint32_t x, int s){
    return x << s | x >> (32 - s);
}

void tea_transform(uint32_t buf[4], const uint32_t in[4]){
    uint32_t sum = 0;
    uint32_t b0 = buf[0], b1 = buf[1];
    uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
    for(int n = 16; n; n--){
        sum += 0x9E3779B9u;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    }
    buf[0] += b0;
    buf[1] += b1;
}

#define MD4_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD4_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD4_ROUND(f, a, b, c, d, x, s) (a += f(b, c, d) + (x), a = rol32(a, s))

void half_md4_transform(uint32_t buf[4], const uint32_t in[8]){
    const uint32_t K2 = 0x5A827999u;
    const uint32_t K3 = 0x6ED9EBA1u;
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
    MD4_ROUND(MD4_F, a, b, c, d, in[0], 3);
    MD4_ROUND(MD4_F, d, a, b, c, in[1], 7);
    MD4_ROUND(MD4_F, c, d, a, b, in[2], 11);
    MD4_ROUND(MD4_F, b, c, d, a, in[3], 19);
    MD4_ROUND(MD4_F, a, b, c, d, in[4], 3);
    MD4_ROUND(MD4_F, d, a, b, c, in[5], 7);
    MD4_ROUND(MD4_F, c, d, a, b, in[6], 11);
    MD4_ROUND(MD4_F, b, c, d, a, in[7], 19);

    MD4_ROUND(MD4_G, a, b, c, d, in[1] + K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, in[3] + K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, in[5] + K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, in[7] + K2, 13);
    MD4_ROUND(MD4_G, a, b, c, d, in[0] + K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, in[2] + K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, in[4] + K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, in[6] + K2, 13);

    MD4_ROUND(MD4_H, a, b, c, d, in[3] + K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, in[7] + K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, in[2] + K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, in[6] + K3, 15);
    MD4_ROUND(MD4_H, a, b, c, d, in[1] + K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, in[5] + K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, in[0] + K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, in[4] + K3, 15);

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

// Caracterele cu semn (char pe x86) sau fara, dupa cum a facut mke2fs / kernelul care a scris indexul
uint32_t chr(const char* s, bool uns){
    return uns ? (uint32_t)(uint8_t)*s : (uint32_t)(int32_t)(int8_t)*s;
}

uint32_t hash_legacy(const char* name, int len, bool uns){
    uint32_t hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
    while(len--){
        uint32_t hash = hash1 + (hash0 ^ (chr(name++, uns) * 7152373u));
        if(hash & 0x80000000u){
            hash -= 0x7fffffffu;
        }
        hash1 = hash0;
        hash0 = hash;
    }
    return hash0 << 1;
}

void str2hashbuf(const char* msg, int len, uint32_t* buf, int num, bool uns){
    uint32_t pad = (uint32_t)len | (uint32_t)len << 8;
    pad |= pad << 16;
    uint32_t val = pad;
    if(len > num * 4){
        len = num * 4;
    }
    for(int i = 0; i < len; i++){
        val = chr(msg + i, uns) + (val << 8);
        if((i % 4) == 3){
            *buf++ = val;
            val = pad;
            num--;
        }
    }
    if(--num >= 0){
        *buf++ = val;
    }
    while(--num >= 0){
        *buf++ = pad;
    }
}

// ver: 0 legacy, 1 half_md4, 2 tea, 3..5 aceleasi cu char fara semn. false = versiune necunoscuta
bool name_hash(Volume* v, uint32_t ver, const char* name, uint32_t len, uint32_t& out){
    uint32_t buf[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    for(int i = 0; i < 4; i++){
        if(v->hash_seed[i]){
            memcpy(buf, v->hash_seed, sizeof(buf));
            break;
        }
    }
    bool uns = v->hash_unsigned;
    if(ver >= 3 && ver <= 5){
        uns = true;
        ver -= 3;
    }
    uint32_t hash;
    uint32_t in[8];
    const char* p = name;
    int left = (int)len;
    switch(ver){
    case 0:
        hash = hash_legacy(name, (int)len, uns);
        break;
    case 1:
        while(left > 0){
            str2hashbuf(p, left, in, 8, uns);
            half_md4_transform(buf, in);
            left -= 32;
            p += 32;
        }
        hash = buf[1];
        break;
    case 2:
        while(left > 0){
            str2hashbuf(p, left, in, 4, uns);
            tea_transform(buf, in);
            left -= 16;
            p += 16;
        }
        hash = buf[0];
        break;
    default:
        return false;
    }
    hash &= ~1u;
    if(hash == (0x7fffffffu << 1)){
        hash = (0x7fffffffu - 1) << 1;
    }
    out = hash;
    return true;
}

// ---- htree ----

const uint32_t DX_MAX_FRAMES = 4;

struct DxFrame {
    uint32_t lblk;
    uint32_t off;                   // inceputul intrarilor (count/limit) in bloc
    uint32_t count;
    uint32_t pos;                   // intrarea aleasa
};

// Nodul dx din blocul lblk: intrarile (count/limit + block al intrarii 0, apoi hash/block). 0 ok, 1 = structura
// neasteptata (se cade pe scanare liniara), <0 eroare de I/O. Pinned in b
int dx_load(Node* d, uint32_t lblk, uint32_t off, Buf*& b, const uint8_t*& ent, uint32_t& count){
    Volume* v = d->vol;
    uint8_t* p = dir_block(d, lblk, b);
    if(!p){
        return -EIO;
    }
    uint32_t limit = le16(p + off);
    count = le16(p + off + 2);
    if(count == 0 || count > limit || off + limit * 8 > v->bs){
        brelse(b);
        b = nullptr;
        return 1;
    }
    if(off == 8 && !(le32(p) == 0 && le16(p + 4) == v->bs)){
        brelse(b);
        b = nullptr;
        return 1;
    }
    ent = p + off;
    return 0;
}

uint32_t dx_hash(const uint8_t* ent, uint32_t i){
    return i == 0 ? 0 : le32(ent + 8 * i);
}

uint32_t dx_block(const uint8_t* ent, uint32_t i){
    return le32(ent + 8 * i + 4) & 0x0fffffffu;
}

// 0 gasit, -ENOENT, 1 = indexul nu se poate folosi (cauta liniar), <0 eroare
int dx_lookup(Node* d, const char* name, uint32_t len, uint32_t& ino){
    Volume* v = d->vol;
    Buf* b;
    uint8_t* p = dir_block(d, 0, b);
    if(!p){
        return -EIO;
    }
    // Blocul 0: "." (rec 12), ".." (rec = restul), apoi dx_root_info: ver(4) info_length(5) niveluri(6)
    bool ok = le16(p + 4) == 12 && le16(p + 16) == v->bs - 12 && p[24 + 5] == 8 && le32(p + 24) == 0;
    uint32_t ver = p[24 + 4];
    uint32_t levels = p[24 + 6];
    brelse(b);
    if(!ok || levels >= DX_MAX_FRAMES || ver > 5){
        return 1;
    }
    uint32_t h;
    if(!name_hash(v, ver, name, len, h)){
        return 1;
    }
    uint32_t nblocks = dir_blocks(d);
    DxFrame fr[DX_MAX_FRAMES];
    uint32_t lblk = 0;
    uint32_t off = 32;
    uint32_t leaf = 0;
    for(uint32_t l = 0;; l++){
        const uint8_t* ent;
        uint32_t count;
        int rc = dx_load(d, lblk, off, b, ent, count);
        if(rc){
            return rc;
        }
        uint32_t pos = 0;
        for(uint32_t i = 1; i < count && dx_hash(ent, i) <= h; i++){
            pos = i;
        }
        fr[l] = {lblk, off, count, pos};
        uint32_t child = dx_block(ent, pos);
        brelse(b);
        if(child >= nblocks){
            return -EIO;
        }
        if(l == levels){
            leaf = child;
            break;
        }
        lblk = child;
        off = 8;
    }
    for(;;){
        int rc = scan_block(d, leaf, name, len, ino);
        if(rc != -ENOENT){
            return rc;
        }
        // Coliziuni de hash pot continua in frunza urmatoare (hash-ul intrarii urmatoare are bitul 0 setat)
        int l = (int)levels;
        while(l >= 0 && fr[l].pos + 1 >= fr[l].count){
            l--;
        }
        if(l < 0){
            return -ENOENT;
        }
        const uint8_t* ent;
        uint32_t count;
        rc = dx_load(d, fr[l].lblk, fr[l].off, b, ent, count);
        if(rc){
            return rc;
        }
        uint32_t nh = dx_hash(ent, fr[l].pos + 1);
        uint32_t child = dx_block(ent, fr[l].pos + 1);
        brelse(b);
        if((nh & ~1u) != h){
            return -ENOENT;
        }
        fr[l].pos++;
        for(uint32_t k = (uint32_t)l + 1; k <= levels; k++){
            if(child >= nblocks){
                return -EIO;
            }
            rc = dx_load(d, child, 8, b, ent, count);
            if(rc){
                return rc;
            }
            fr[k] = {child, 8, count, 0};
            child = dx_block(ent, 0);
            brelse(b);
        }
        if(child >= nblocks){
            return -EIO;
        }
        leaf = child;
    }
}

}

int dir_lookup(Node* d, const char* name, uint32_t len, uint32_t& ino){
    if(len == 0 || len > NAME_MAX){
        return -ENOENT;
    }
    if(d->flags & INODE_INDEX_FL){
        int rc = dx_lookup(d, name, len, ino);
        if(rc <= 0){
            stat_htree++;
            return rc;
        }
    }
    return linear_lookup(d, name, len, ino);
}

int dir_readdir(Node* d, uint64_t index, DirEntry& out){
    Volume* v = d->vol;
    uint64_t vis = 0;
    uint32_t lblk = 0;
    uint32_t off = 0;
    if(d->rd_valid && index >= d->rd_idx){
        vis = d->rd_idx;
        lblk = d->rd_lblk;
        off = d->rd_off;
    }
    uint32_t nblocks = dir_blocks(d);
    for(; lblk < nblocks; lblk++, off = 0){
        Buf* b;
        uint8_t* blk = dir_block(d, lblk, b);
        if(!blk){
            return -EIO;
        }
        while(off < v->bs){
            Dirent e;
            if(!dirent_at(blk, v->bs, off, e)){
                brelse(b);
                return -EIO;
            }
            off += e.rec;
            if(!e.ino || !e.nlen || (e.name[0] == '.' && (e.nlen == 1 || (e.nlen == 2 && e.name[1] == '.')))){
                continue;
            }
            if(vis++ == index){
                memcpy(out.name, e.name, e.nlen);
                out.name[e.nlen] = 0;
                out.name_len = (uint8_t)e.nlen;
                out.type = dtype_of(v, e.type);
                out.ino = e.ino;
                brelse(b);
                d->rd_valid = true;
                d->rd_idx = vis;
                d->rd_lblk = lblk;
                d->rd_off = off;
                return 1;
            }
        }
        brelse(b);
    }
    return 0;
}

}
