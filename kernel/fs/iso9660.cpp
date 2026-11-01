#include "fs/iso9660.h"
#include "fs/vfs/vfs.h"
#include "block/blockdev.h"
#include "block/bcache.h"
#include "hal/hal.h"
#include "sync/mutex.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

namespace {

const uint32_t SECTOR = 2048;               // sectorul logic ECMA-119: inregistrarile de director nu il depasesc
const uint64_t PVD_OFF = 16ull * SECTOR;
const uint32_t LINK_CAP = 1024;
const int CE_DEPTH = 8;

struct Node;

struct Sb {
    SuperBlock* vsb;
    BlockDevice* dev;
    uint32_t lbs;                           // dimensiunea blocului logic (de obicei 2048)
    uint64_t dev_bytes;
    bool use_rr;
    bool has_rr;
    Mutex lock;                             // cursoarele de readdir ale directoarelor
    Node* root;
    Sb* next;
};

struct Node {
    Sb* sb;
    Inode* ino;
    uint64_t start;                         // octetul de pe dispozitiv unde incep datele
    uint64_t size;
    char* link;
    uint32_t link_len;
    bool rd_valid;
    bool rd_skip;
    uint64_t rd_index;
    uint64_t rd_pos;
};

// O inregistrare de director, impreuna cu ce s-a gasit in zona System Use
struct Rec {
    uint64_t rec_off;                       // octetul inregistrarii pe dispozitiv (da numarul de inode)
    uint64_t start;
    uint64_t size;
    uint8_t flags;
    bool is_dot;
    bool is_dotdot;
    bool rr_name;
    bool has_px;
    bool has_tf;
    bool has_cl;
    bool re;
    bool link_cont;
    uint32_t cl_lba;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    Timespec rec_time;
    Timespec at;
    Timespec mt;
    Timespec ct;
    uint32_t nlen;
    uint32_t link_len;
    char name[NAME_MAX + 1];
    char link[LINK_CAP];
};

extern const InodeOps iso_ops;
extern const SuperBlockOps iso_sb_ops;

Sb* sbs = nullptr;

uint16_t le16(const uint8_t* p){
    return (uint16_t)(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t* p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

char lower(char c){
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

// ---- citire prin cache-ul de blocuri ----

int iso_read(Sb* s, uint64_t off, void* buf, uint64_t len){
    if(off > s->dev_bytes || len > s->dev_bytes - off){
        return -EIO;
    }
    uint8_t* out = (uint8_t*)buf;
    while(len){
        uint64_t blk = off / BCACHE_BLOCK;
        uint64_t boff = off % BCACHE_BLOCK;
        uint64_t n = BCACHE_BLOCK - boff < len ? BCACHE_BLOCK - boff : len;
        if((blk + 1) * BCACHE_BLOCK <= s->dev_bytes){
            Buf* b = bread(s->dev, blk);
            if(!b){
                return -EIO;
            }
            memcpy(out, b->data + boff, n);
            brelse(b);
        } else {
            // Ultimul bloc de 4 KiB al unui CD cu numar impar de sectoare de 2 KiB: citire directa
            int err = block_read_bytes(s->dev, off, out, n);
            if(err){
                return err;
            }
        }
        off += n;
        out += n;
        len -= n;
    }
    return 0;
}

// ---- timp ----

int64_t days_from_civil(int64_t y, int64_t m, int64_t d){
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Timespec make_time(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t sec, int64_t gmt15){
    Timespec t;
    if(mo < 1 || mo > 12 || d < 1 || d > 31){
        t.sec = 0;
        t.nsec = 0;
        return t;
    }
    t.sec = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - gmt15 * 900;
    t.nsec = 0;
    return t;
}

Timespec time7(const uint8_t* p){
    return make_time(1900 + p[0], p[1], p[2], p[3], p[4], p[5], (int8_t)p[6]);
}

int dig(const uint8_t* p, int n){
    int v = 0;
    for(int i = 0; i < n; i++){
        v = v * 10 + (p[i] >= '0' && p[i] <= '9' ? p[i] - '0' : 0);
    }
    return v;
}

Timespec time17(const uint8_t* p){
    Timespec t = make_time(dig(p, 4), dig(p + 4, 2), dig(p + 6, 2), dig(p + 8, 2), dig(p + 10, 2), dig(p + 12, 2),
                           (int8_t)p[16]);
    t.nsec = dig(p + 14, 2) * 10000000ll;
    return t;
}

// ---- zona System Use (SUSP / Rock Ridge) ----

void link_add(Rec& r, const char* s, uint32_t n){
    for(uint32_t i = 0; i < n && r.link_len + 1 < LINK_CAP; i++){
        r.link[r.link_len++] = s[i];
    }
}

void link_sep(Rec& r){
    if(!r.link_cont && r.link_len > 0){
        link_add(r, "/", 1);
    }
}

void parse_sl(Rec& r, const uint8_t* d, uint32_t dl){
    uint32_t i = 1;                                 // d[0] = flags (CONTINUE: urmeaza alt SL, ne intereseaza doar componentele)
    while(i + 2 <= dl){
        uint8_t cf = d[i];
        uint8_t cl = d[i + 1];
        if(i + 2 + cl > dl){
            break;
        }
        const char* c = (const char*)d + i + 2;
        if(cf & 8){
            link_add(r, "/", 1);                    // ROOT
            r.link_cont = true;
        } else {
            link_sep(r);
            if(cf & 2){
                link_add(r, ".", 1);
            } else if(cf & 4){
                link_add(r, "..", 2);
            } else {
                link_add(r, c, cl);
            }
            r.link_cont = cf & 1;
        }
        i += 2 + cl;
    }
    r.link[r.link_len] = 0;
}

int parse_su(Sb* s, const uint8_t* p, uint32_t n, Rec& r, int depth){
    while(n >= 4){
        uint8_t len = p[2];
        if(len < 4 || len > n){
            break;
        }
        const uint8_t* d = p + 4;
        uint32_t dl = len - 4u;
        char a = (char)p[0];
        char b = (char)p[1];
        if(a == 'N' && b == 'M' && dl >= 1){
            if(d[0] & 2){
                r.is_dot = true;
            } else if(d[0] & 4){
                r.is_dotdot = true;
            } else {
                for(uint32_t i = 1; i < dl && r.nlen < NAME_MAX; i++){
                    r.name[r.nlen++] = (char)d[i];
                }
                r.rr_name = true;
            }
        } else if(a == 'P' && b == 'X' && dl >= 32){
            r.has_px = true;
            r.mode = le32(d);
            r.nlink = le32(d + 8);
            r.uid = le32(d + 16);
            r.gid = le32(d + 24);
        } else if(a == 'T' && b == 'F' && dl >= 1){
            uint8_t f = d[0];
            uint32_t sz = (f & 0x80) ? 17 : 7;
            const uint8_t* q = d + 1;
            uint32_t left = dl - 1;
            for(int bit = 0; bit < 7 && left >= sz; bit++){
                if(!(f & (1 << bit))){
                    continue;
                }
                Timespec t = (f & 0x80) ? time17(q) : time7(q);
                if(bit == 0 && !r.has_tf){
                    r.ct = t;
                } else if(bit == 1){
                    r.mt = t;
                } else if(bit == 2){
                    r.at = t;
                } else if(bit == 3){
                    r.ct = t;
                }
                r.has_tf = true;
                q += sz;
                left -= sz;
            }
        } else if(a == 'S' && b == 'L' && dl >= 1){
            parse_sl(r, d, dl);
        } else if(a == 'C' && b == 'L' && dl >= 8){
            r.has_cl = true;
            r.cl_lba = le32(d);
        } else if(a == 'R' && b == 'E'){
            r.re = true;
        } else if(a == 'S' && b == 'P' && dl >= 3){
            if(d[0] == 0xBE && d[1] == 0xEF){
                s->has_rr = true;
            }
        } else if(a == 'C' && b == 'E' && dl >= 24 && depth < CE_DEPTH){
            uint64_t blk = le32(d);
            uint32_t off = le32(d + 8);
            uint32_t cl = le32(d + 16);
            if(cl && cl <= 4096 && off <= 4096 && blk * s->lbs + off + cl <= s->dev_bytes){
                uint8_t* tmp = (uint8_t*)kmalloc(cl);
                if(tmp){
                    if(iso_read(s, blk * s->lbs + off, tmp, cl) == 0){
                        parse_su(s, tmp, cl, r, depth + 1);
                    }
                    kfree(tmp);
                }
            }
        } else if(a == 'S' && b == 'T'){
            break;
        }
        p += len;
        n -= len;
    }
    return 0;
}

// Nume ISO fara Rock Ridge: fara ";versiune", fara punctul final, litere mici
void iso_name(Rec& r, const uint8_t* id, uint32_t idlen){
    uint32_t n = idlen;
    for(uint32_t i = n; i > 0; i--){
        if(id[i - 1] == ';'){
            n = i - 1;
            break;
        }
    }
    if(n > 0 && id[n - 1] == '.'){
        n--;
    }
    if(n > NAME_MAX){
        n = NAME_MAX;
    }
    for(uint32_t i = 0; i < n; i++){
        r.name[i] = lower((char)id[i]);
    }
    r.nlen = n;
}

// rec: inregistrarea in memorie (len = rec[0] >= 34 verificat de apelant)
int parse_rec(Sb* s, const uint8_t* rec, uint64_t rec_off, Rec& r){
    memset(&r, 0, __builtin_offsetof(Rec, name));
    r.nlen = 0;
    r.link_len = 0;
    r.link[0] = 0;
    uint32_t len = rec[0];
    r.rec_off = rec_off;
    r.start = ((uint64_t)le32(rec + 2) + rec[1]) * s->lbs;
    r.size = le32(rec + 10);
    r.flags = rec[25];
    r.rec_time = time7(rec + 18);
    uint32_t idlen = rec[32];
    if(33 + idlen > len){
        return -EIO;
    }
    const uint8_t* id = rec + 33;
    if(idlen == 1 && id[0] == 0){
        r.is_dot = true;
    } else if(idlen == 1 && id[0] == 1){
        r.is_dotdot = true;
    }
    uint32_t su = 33 + idlen + ((idlen & 1) ? 0u : 1u);
    if(s->use_rr && su + 4 <= len){
        parse_su(s, rec + su, len - su, r, 0);
    }
    if(!r.rr_name && !r.is_dot && !r.is_dotdot){
        iso_name(r, id, idlen);
    } else if(r.rr_name){
        r.name[r.nlen] = 0;
    }
    if(r.is_dot || r.is_dotdot){
        return 0;
    }
    if(r.has_cl){
        // Director relocat: locul real e in inregistrarea "." de la cl_lba
        uint8_t dot[34];
        uint64_t at = (uint64_t)r.cl_lba * s->lbs;
        int err = iso_read(s, at, dot, sizeof(dot));
        if(err){
            return err;
        }
        if(dot[0] >= 34){
            r.start = ((uint64_t)le32(dot + 2) + dot[1]) * s->lbs;
            r.size = le32(dot + 10);
        }
        r.flags |= 2;
    }
    return 0;
}

uint32_t rec_mode(const Rec& r){
    uint32_t m;
    if(r.has_px){
        m = r.mode & (S_IFMT | 07777);
        if(r.flags & 2){
            m = (m & ~(uint32_t)S_IFMT) | S_IFDIR;
        }
        if(!(m & S_IFMT)){
            m |= S_IFREG;
        }
    } else {
        m = (r.flags & 2) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    }
    if(r.has_cl){
        m = (m & ~(uint32_t)S_IFMT) | S_IFDIR;
    }
    return m;
}

bool rec_hidden(const Rec& r){
    if(r.is_dot || r.is_dotdot || r.re || r.nlen == 0){
        return true;
    }
    uint32_t t = rec_mode(r) & S_IFMT;
    return t != S_IFREG && t != S_IFDIR && t != S_IFLNK;
}

// ---- iterare peste un director ----

struct DirIt {
    Sb* s;
    uint64_t base;
    uint64_t size;
    uint64_t pos;
    uint8_t* sec;
    uint64_t sec_idx;
    bool have;
    bool skip_multi;
};

// 1 = inregistrare (rec pointeaza in it.sec), 0 = sfarsit, <0 = eroare
int dir_next(DirIt& it, const uint8_t*& rec, uint64_t& rec_off){
    for(;;){
        if(it.pos >= it.size){
            return 0;
        }
        uint64_t sidx = it.pos / SECTOR;
        if(!it.have || it.sec_idx != sidx){
            int err = iso_read(it.s, it.base + sidx * SECTOR, it.sec, SECTOR);
            if(err){
                return err;
            }
            it.have = true;
            it.sec_idx = sidx;
        }
        uint32_t o = (uint32_t)(it.pos % SECTOR);
        uint32_t len = it.sec[o];
        if(len == 0 || len < 34 || o + len > SECTOR){
            it.pos = (sidx + 1) * SECTOR;
            continue;
        }
        rec = it.sec + o;
        rec_off = it.base + it.pos;
        it.pos += len;
        bool cont = it.skip_multi;
        it.skip_multi = rec[25] & 0x80;
        if(cont){
            continue;                               // continuare de fisier multi-extent
        }
        return 1;
    }
}

void it_init(DirIt& it, Sb* s, Node* d, uint8_t* sec){
    it.s = s;
    it.base = d->start;
    it.size = d->size;
    it.pos = 0;
    it.sec = sec;
    it.sec_idx = 0;
    it.have = false;
    it.skip_multi = false;
}

// ---- inode-uri ----

Node* node_of(const Inode* i){
    return (Node*)i->priv;
}

Inode* make_inode(Sb* s, const Rec& r, int& err){
    uint32_t mode = rec_mode(r);
    Node* n = (Node*)kzalloc(sizeof(Node));
    Inode* ino = n ? inode_alloc(s->vsb, r.rec_off, mode, &iso_ops) : nullptr;
    if(!ino){
        kfree(n);
        err = -ENOMEM;
        return nullptr;
    }
    n->sb = s;
    n->ino = ino;
    n->start = r.start;
    n->size = r.size;
    if(S_ISLNK(mode)){
        n->link = (char*)kmalloc(r.link_len + 1);
        if(!n->link){
            inode_free(ino);
            kfree(n);
            err = -ENOMEM;
            return nullptr;
        }
        memcpy(n->link, r.link, r.link_len);
        n->link[r.link_len] = 0;
        n->link_len = r.link_len;
        n->size = r.link_len;
    }
    ino->priv = n;
    ino->size = n->size;
    ino->uid = r.has_px ? r.uid : 0;
    ino->gid = r.has_px ? r.gid : 0;
    ino->nlink = r.has_px && r.nlink ? r.nlink : (S_ISDIR(mode) ? 2u : 1u);
    ino->atime = r.has_tf && r.at.sec ? r.at : r.rec_time;
    ino->mtime = r.has_tf && r.mt.sec ? r.mt : r.rec_time;
    ino->ctime = r.has_tf && r.ct.sec ? r.ct : r.rec_time;
    err = 0;
    return ino;
}

bool name_match(const Rec& r, const char* name, uint32_t len){
    if(r.nlen != len){
        return false;
    }
    if(r.rr_name){
        return memcmp(r.name, name, len) == 0;
    }
    for(uint32_t i = 0; i < len; i++){
        if(lower(r.name[i]) != lower(name[i])){
            return false;
        }
    }
    return true;
}

int iso_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    Node* d = node_of(dir);
    Sb* s = d->sb;
    Rec* r = (Rec*)kmalloc(sizeof(Rec));
    uint8_t* sec = (uint8_t*)kmalloc(SECTOR);
    int err = -ENOMEM;
    if(r && sec){
        DirIt it;
        it_init(it, s, d, sec);
        err = -ENOENT;
        for(;;){
            const uint8_t* rec;
            uint64_t roff;
            int rc = dir_next(it, rec, roff);
            if(rc <= 0){
                err = rc < 0 ? rc : -ENOENT;
                break;
            }
            if(parse_rec(s, rec, roff, *r) || rec_hidden(*r) || !name_match(*r, name, len)){
                continue;
            }
            Inode* ino = make_inode(s, *r, err);
            if(ino){
                out = ino;
            }
            break;
        }
    }
    kfree(r);
    kfree(sec);
    return err;
}

int iso_readdir(Inode* dir, uint64_t index, DirEntry& out){
    Node* d = node_of(dir);
    Sb* s = d->sb;
    Rec* r = (Rec*)kmalloc(sizeof(Rec));
    uint8_t* sec = (uint8_t*)kmalloc(SECTOR);
    int result = -ENOMEM;
    if(r && sec){
        MutexGuard g(s->lock);
        DirIt it;
        it_init(it, s, d, sec);
        uint64_t vis = 0;
        if(d->rd_valid && d->rd_index == index){
            it.pos = d->rd_pos;
            it.skip_multi = d->rd_skip;
            vis = index;
        }
        result = 0;
        for(;;){
            const uint8_t* rec;
            uint64_t roff;
            int rc = dir_next(it, rec, roff);
            if(rc <= 0){
                result = rc;
                d->rd_valid = false;
                break;
            }
            if(parse_rec(s, rec, roff, *r) || rec_hidden(*r)){
                continue;
            }
            if(vis == index){
                uint32_t m = rec_mode(*r);
                memcpy(out.name, r->name, r->nlen);
                out.name[r->nlen] = 0;
                out.name_len = (uint8_t)r->nlen;
                out.ino = r->rec_off;
                out.type = S_ISDIR(m) ? DT_DIR : S_ISLNK(m) ? DT_LNK : DT_REG;
                d->rd_valid = true;
                d->rd_index = index + 1;
                d->rd_pos = it.pos;
                d->rd_skip = it.skip_multi;
                result = 1;
                break;
            }
            vis++;
        }
    }
    kfree(r);
    kfree(sec);
    return result;
}

int64_t iso_file_read(Inode* ino, uint64_t pos, void* buf, uint64_t len){
    Node* n = node_of(ino);
    if(pos >= n->size){
        return 0;
    }
    if(len > n->size - pos){
        len = n->size - pos;
    }
    int err = iso_read(n->sb, n->start + pos, buf, len);
    return err ? err : (int64_t)len;
}

int64_t iso_readlink(Inode* ino, char* buf, uint64_t len){
    Node* n = node_of(ino);
    if(!n->link){
        return -EINVAL;
    }
    uint64_t k = n->link_len < len ? n->link_len : len;
    memcpy(buf, n->link, k);
    return (int64_t)k;
}

int iso_create(Inode*, const char*, uint32_t, uint32_t, Inode*&){ return -EROFS; }
int iso_symlink(Inode*, const char*, uint32_t, const char*, Inode*&){ return -EROFS; }
int iso_unlink(Inode*, const char*, uint32_t, Inode*){ return -EROFS; }
int64_t iso_write(Inode*, uint64_t, const void*, uint64_t){ return -EROFS; }
int iso_truncate(Inode*, uint64_t){ return -EROFS; }
int iso_rename(Inode*, const char*, uint32_t, Inode*, const char*, uint32_t, Inode*, Inode*){ return -EROFS; }

const InodeOps iso_ops = {iso_lookup, iso_create, iso_create, iso_symlink, iso_unlink, iso_unlink, iso_file_read,
                          iso_write, iso_truncate, iso_readdir, iso_readlink, iso_rename};

// ---- sistemul de fisiere ----

void iso_release(Inode* i){
    Node* n = node_of(i);
    if(n == n->sb->root){
        return;                                 // ramane al volumului pana la kill
    }
    kfree(n->link);
    inode_free(i);
    kfree(n);
}

void iso_kill(SuperBlock* vsb){
    Sb* s = (Sb*)vsb->priv;
    for(Sb** pp = &sbs; *pp; pp = &(*pp)->next){
        if(*pp == s){
            *pp = s->next;
            break;
        }
    }
    bcache().invalidate(s->dev);
    if(s->root){
        inode_free(s->root->ino);
        kfree(s->root);
    }
    kfree(s);
}

const SuperBlockOps iso_sb_ops = {iso_release, iso_kill};

BlockDevice* find_block(const char* name){
    if(name && memcmp(name, "/dev/", 5) == 0){
        name += 5;
    }
    for(int k = 0; name && k < hal_count(HalKind::Block); k++){
        const char* hn = hal_name(HalKind::Block, k);
        const char* a = name;
        while(*a && *a == *hn){
            a++;
            hn++;
        }
        if(!*a && !*hn){
            return hal_block(k);
        }
    }
    return nullptr;
}

int iso_mount(FileSystemType*, SuperBlock* vsb, const char* dev_name, const char* data){
    BlockDevice* dev = find_block(dev_name);
    if(!dev){
        return -ENODEV;
    }
    bool use_rr = true;
    while(data && *data){
        if(data[0] == 'r' && data[1] == 'o' && (data[2] == ',' || !data[2])){
        } else if(memcmp(data, "norr", 4) == 0 && (data[4] == ',' || !data[4])){
            use_rr = false;
        } else if(data[0] != ','){
            return -EINVAL;
        }
        while(*data && *data != ','){
            data++;
        }
        if(*data == ','){
            data++;
        }
    }
    uint64_t dev_bytes = dev->block_count() * (uint64_t)dev->block_size();
    void* mem = kzalloc(sizeof(Sb));
    uint8_t* sec = (uint8_t*)kmalloc(SECTOR);
    if(!mem || !sec){
        kfree(mem);
        kfree(sec);
        return -ENOMEM;
    }
    Sb* s = new (mem) Sb();
    s->vsb = vsb;
    s->dev = dev;
    s->dev_bytes = dev_bytes;
    s->use_rr = use_rr;

    // Descriptorii de volum incep la 32768; cautam PVD pana la terminator
    int err = -EINVAL;
    uint8_t pvd[SECTOR];
    for(uint32_t i = 0; i < 32; i++){
        int rc = iso_read(s, PVD_OFF + (uint64_t)i * SECTOR, sec, SECTOR);
        if(rc){
            err = rc == -EIO ? -EINVAL : rc;
            break;
        }
        if(memcmp(sec + 1, "CD001", 5) != 0 || sec[6] != 1){
            break;
        }
        if(sec[0] == 1){
            memcpy(pvd, sec, SECTOR);
            err = 0;
            break;
        }
        if(sec[0] == 255){
            break;
        }
    }
    kfree(sec);
    if(err){
        kfree(s);
        return err;
    }
    uint32_t lbs = le16(pvd + 128);
    if(lbs < 512 || lbs > 4096 || (lbs & (lbs - 1))){
        kfree(s);
        return -EINVAL;
    }
    s->lbs = lbs;
    const uint8_t* rr = pvd + 156;
    if(rr[0] < 34 || !(rr[25] & 2)){
        kfree(s);
        return -EINVAL;
    }
    uint64_t rstart = ((uint64_t)le32(rr + 2) + rr[1]) * lbs;
    uint64_t rsize = le32(rr + 10);
    if(rsize == 0 || rstart > dev_bytes || rsize > dev_bytes - rstart){
        kfree(s);
        return -EINVAL;
    }

    // Radacina: atributele (PX/TF) sunt in inregistrarea "." de la inceputul directorului
    Rec* r = (Rec*)kmalloc(sizeof(Rec));
    uint8_t* first = (uint8_t*)kmalloc(SECTOR);
    if(!r || !first){
        kfree(r);
        kfree(first);
        kfree(s);
        return -ENOMEM;
    }
    memset(r, 0, __builtin_offsetof(Rec, name));
    err = iso_read(s, rstart, first, SECTOR);
    if(!err){
        if(first[0] >= 34){
            err = parse_rec(s, first, rstart, *r);
        }
        r->start = rstart;
        r->size = rsize;
        r->flags = 2;
        r->rec_off = rstart;
        r->has_cl = false;
        r->is_dot = false;
        r->nlen = 0;
    }
    kfree(first);
    if(err){
        kfree(r);
        kfree(s);
        return err == -EIO ? -EINVAL : err;
    }
    r->link_len = 0;
    int merr;
    Inode* ri = make_inode(s, *r, merr);
    kfree(r);
    if(!ri){
        kfree(s);
        return merr;
    }
    ri->mode = (ri->mode & ~(uint32_t)S_IFMT) | S_IFDIR;
    s->root = node_of(ri);
    vsb->priv = s;
    vsb->ops = &iso_sb_ops;
    vsb->flags |= MS_RDONLY | SB_NOCACHE;
    vsb->root_inode = ri;
    s->next = sbs;
    sbs = s;
    return 0;
}

FileSystemType iso_type = {"iso9660", iso_mount, nullptr};

}

void iso9660_init(){
    fs_register(&iso_type);
}

int iso9660_automount(){
    int last = -ENODEV;
    for(int k = 0; k < hal_count(HalKind::Block); k++){
        const char* name = hal_name(HalKind::Block, k);
        if(!name || name[0] != 's' || name[1] != 'r'){
            continue;
        }
        Path none = {};
        int err = vfs_mount(none, name, "/media/cdrom", "iso9660", MS_RDONLY, nullptr);
        if(err){
            last = err;
            if(err != -EINVAL){
                kprintf("iso9660: montarea lui %s pe /media/cdrom a esuat (%d)\n", name, err);
            }
            continue;
        }
        kprintf("[OK] iso9660 montat pe /media/cdrom (%s)\n", name);
        return 0;
    }
    return last;
}
