#include "fs/fat32/fat32.h"
#include "fs/fat32/fat32_priv.h"
#include "fs/vfs/vfs.h"
#include "block/blockdev.h"
#include "hal/hal.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

using namespace fat32;

namespace {

// Volumele montate (modificate doar sub lock-ul VFS, in mount / kill)
Volume* volumes = nullptr;

extern const InodeOps fat_ops;
extern const SuperBlockOps fat_sb_ops;

Node* node_of(const Inode* i){
    return (Node*)i->priv;
}

Volume* vol_of(const Inode* i){
    return (Volume*)i->sb->priv;
}

uint16_t le16(const uint8_t* p){
    return (uint16_t)(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t* p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void put16(uint8_t* p, uint16_t v){
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void put32(uint8_t* p, uint32_t v){
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

// ---- BPB ----

struct Bpb {
    uint32_t bps, spc, reserved, nfats, fat_sectors, total, root_cluster, fsinfo, ext_flags;
};

bool pow2(uint32_t x){
    return x && !(x & (x - 1));
}

bool bpb_parse(const uint8_t* s, Bpb& b){
    if(s[510] != 0x55 || s[511] != 0xAA){
        return false;
    }
    b.bps = le16(s + 11);
    b.spc = s[13];
    b.reserved = le16(s + 14);
    b.nfats = s[16];
    uint32_t root_entries = le16(s + 17);
    uint32_t total16 = le16(s + 19);
    uint32_t fat16 = le16(s + 22);
    uint32_t total32 = le32(s + 32);
    b.fat_sectors = le32(s + 36);
    b.ext_flags = le16(s + 40);
    uint32_t version = le16(s + 42);
    b.root_cluster = le32(s + 44);
    b.fsinfo = le16(s + 48);
    b.total = total16 ? total16 : total32;
    if(!pow2(b.bps) || b.bps < 512 || b.bps > 4096 || !pow2(b.spc) || b.spc > 128){
        return false;
    }
    if(b.reserved < 1 || b.nfats < 1 || b.nfats > 2 || root_entries != 0 || fat16 != 0 || b.fat_sectors == 0 ||
       version != 0 || b.total == 0 || b.root_cluster < 2){
        return false;
    }
    return true;
}

// ---- noduri ----

uint64_t synth_ino(uint32_t first, uint32_t dir_first, uint32_t ent_off){
    return first ? first : (1ULL << 63) | (uint64_t)dir_first << 32 | ent_off;
}

Node* node_find(Volume* v, uint32_t dir_first, uint32_t ent_off){
    for(Node* n = v->nodes; n; n = n->next){
        if(!n->dead && n != v->root && n->dir_first == dir_first && n->ent_off == ent_off){
            return n;
        }
    }
    return nullptr;
}

void node_link(Volume* v, Node* n){
    n->prev = nullptr;
    n->next = v->nodes;
    if(v->nodes){
        v->nodes->prev = n;
    }
    v->nodes = n;
}

void node_unlink(Volume* v, Node* n){
    if(n->prev){
        n->prev->next = n->next;
    } else {
        v->nodes = n->next;
    }
    if(n->next){
        n->next->prev = n->prev;
    }
}

// Nodul pentru intrarea scurta raw aflata la ent_off in directorul dir_first; nodul viu existent daca e deja cunoscut
Node* node_for(Volume* v, uint32_t dir_first, const uint8_t* raw, uint32_t ent_off, uint32_t lfn_off){
    Node* n = node_find(v, dir_first, ent_off);
    if(n){
        return n;
    }
    uint8_t attr = raw[11];
    bool dir = attr & ATTR_DIR;
    uint32_t first = (uint32_t)le16(raw + 20) << 16 | le16(raw + 26);
    uint32_t mode = dir ? S_IFDIR | ((attr & ATTR_RO) ? 0555 : 0755) : S_IFREG | ((attr & ATTR_RO) ? 0444 : 0644);
    void* mem = kzalloc(sizeof(Node));
    if(!mem){
        return nullptr;
    }
    n = new (mem) Node();
    Inode* i = inode_alloc(v->sb, synth_ino(first, dir_first, ent_off), mode, &fat_ops);
    if(!i){
        kfree(n);
        return nullptr;
    }
    i->priv = n;
    i->nlink = dir ? 2 : 1;
    i->size = dir ? 0 : le32(raw + 28);
    i->ctime = fat_time_decode(le16(raw + 16), le16(raw + 14));
    i->mtime = fat_time_decode(le16(raw + 24), le16(raw + 22));
    i->atime = fat_time_decode(le16(raw + 18), 0);
    n->vol = v;
    n->ino = i;
    n->first = first;
    n->dir_first = dir_first;
    n->ent_off = ent_off;
    n->lfn_off = lfn_off;
    n->attr = attr;
    node_link(v, n);
    return n;
}

void stamp_times(uint8_t* raw, int64_t now){
    uint16_t d, t;
    fat_time_encode(now, d, t);
    put16(raw + 14, t);
    put16(raw + 16, d);
    put16(raw + 18, d);
    put16(raw + 22, t);
    put16(raw + 24, d);
}

// Scrie in intrarea scurta primul cluster, dimensiunea si timpii nodului
int node_flush(Node* n){
    Volume* v = n->vol;
    if(n->dead || v->readonly || n == v->root){
        return 0;
    }
    DirIter it;
    it_init(it, v, n->dir_first);
    uint8_t r[ENT_SIZE];
    int rc = dir_read_raw(it, n->ent_off, r);
    if(rc){
        return rc < 0 ? rc : -EIO;
    }
    bool dir = S_ISDIR(n->ino->mode);
    put16(r + 20, (uint16_t)(n->first >> 16));
    put16(r + 26, (uint16_t)n->first);
    uint16_t d, t;
    if(!dir){
        put32(r + 28, (uint32_t)n->ino->size);
        r[11] |= ATTR_ARCHIVE;
    }
    fat_time_encode(n->ino->mtime.sec, d, t);
    put16(r + 22, t);
    put16(r + 24, d);
    fat_time_encode(n->ino->atime.sec, d, t);
    put16(r + 18, d);
    return dir_write_raw(it, n->ent_off, r);
}

// ---- lanturi de clustere ale unui fisier ----

// Clusterul numarul idx din lantul nodului; cu create adauga clustere (zerorizate) pana acolo. 1 = lantul e mai scurt
int node_cluster(Node* n, uint32_t idx, bool create, uint32_t& clu){
    Volume* v = n->vol;
    if(n->first == 0){
        if(!create){
            return 1;
        }
        uint32_t c;
        int err = clu_alloc(v, 0, c);
        if(err){
            return err;
        }
        n->first = c;
        n->c_idx = 0;
        n->c_clu = c;
    }
    uint32_t i = 0;
    uint32_t c = n->first;
    if(n->c_clu && n->c_idx <= idx){
        i = n->c_idx;
        c = n->c_clu;
    }
    while(i < idx){
        uint32_t nx;
        int err = fat_next(v, c, nx);
        if(err){
            return err;
        }
        if(!nx){
            if(!create){
                return 1;
            }
            err = clu_alloc(v, c, nx);
            if(err){
                return err;
            }
        }
        c = nx;
        i++;
    }
    n->c_idx = i;
    n->c_clu = c;
    clu = c;
    return 0;
}

// Elibereaza clusterele de dincolo de ceil(size / cluster)
int node_trim(Node* n, uint64_t size){
    Volume* v = n->vol;
    n->c_clu = 0;
    if(n->first == 0){
        return 0;
    }
    uint64_t keep = (size + v->cluster_bytes - 1) / v->cluster_bytes;
    if(keep == 0){
        clu_free_chain(v, n->first);
        n->first = 0;
        return 0;
    }
    uint32_t c;
    int rc = node_cluster(n, (uint32_t)keep - 1, false, c);
    if(rc){
        return rc > 0 ? 0 : rc;
    }
    uint32_t nx;
    int err = fat_next(v, c, nx);
    if(err){
        return err;
    }
    if(nx){
        err = fat_set(v, c, FAT_EOC);
        if(err){
            return err;
        }
        clu_free_chain(v, nx);
    }
    n->c_clu = 0;
    return 0;
}

// Zerorizeaza octetii [from, to) ai fisierului, care sunt in clustere deja alocate
int node_zero(Node* n, uint64_t from, uint64_t to){
    Volume* v = n->vol;
    while(from < to){
        uint32_t idx = (uint32_t)(from / v->cluster_bytes);
        uint32_t co = (uint32_t)(from % v->cluster_bytes);
        uint64_t chunk = v->cluster_bytes - co;
        if(chunk > to - from){
            chunk = to - from;
        }
        uint32_t c;
        int rc = node_cluster(n, idx, false, c);
        if(rc){
            return rc > 0 ? -EIO : rc;
        }
        int err = vol_zero(v, clu_offset(v, c) + co, chunk);
        if(err){
            return err;
        }
        from += chunk;
    }
    return 0;
}

// ---- operatii pe inode-uri ----

int name_in(const char* name, uint32_t len, uint16_t* out, uint32_t& n){
    int err = utf8_to_utf16(name, len, out, NAME16_MAX, n);
    if(err){
        return err;
    }
    return name_valid(out, n) ? 0 : -EINVAL;
}

int fat_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    Node* d = node_of(dir);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    uint16_t nm[NAME16_MAX];
    uint32_t nn;
    if(utf8_to_utf16(name, len, nm, NAME16_MAX, nn) || nn == 0){
        return -ENOENT;
    }
    Entry e;
    int err = dir_find(v, d->first, nm, nn, e);
    if(err){
        return err;
    }
    Node* n = node_for(v, d->first, e.raw, e.ent_off, e.lfn_off);
    if(!n){
        return -ENOMEM;
    }
    out = n->ino;
    return 0;
}

// Intrare noua (fisier sau director) in dir; node intors fara referinta. first = clusterul deja alocat (director)
int make_entry(Node* d, const char* name, uint32_t len, bool is_dir, Inode*& out){
    Volume* v = d->vol;
    if(v->readonly){
        return -EROFS;
    }
    uint16_t nm[NAME16_MAX];
    uint32_t nn;
    int err = name_in(name, len, nm, nn);
    if(err){
        return err;
    }
    err = vol_begin_write(v);
    if(err){
        return err;
    }
    Entry ex;
    err = dir_find(v, d->first, nm, nn, ex);
    if(err != -ENOENT){
        return err ? err : -EEXIST;
    }
    uint32_t first = 0;
    int64_t now = vfs_now().sec;
    if(is_dir){
        err = clu_alloc(v, 0, first);
        if(err){
            return err;
        }
        // "." si ".." in clusterul nou (.. = 0 cand parintele e radacina)
        uint8_t e[2 * ENT_SIZE];
        memset(e, 0, sizeof(e));
        memset(e, ' ', 11);
        e[0] = '.';
        memset(e + ENT_SIZE, ' ', 11);
        e[ENT_SIZE] = '.';
        e[ENT_SIZE + 1] = '.';
        for(int k = 0; k < 2; k++){
            uint8_t* p = e + k * ENT_SIZE;
            p[11] = ATTR_DIR;
            stamp_times(p, now);
            uint32_t c = k == 0 ? first : (d == v->root ? 0 : d->first);
            put16(p + 20, (uint16_t)(c >> 16));
            put16(p + 26, (uint16_t)c);
        }
        err = vol_rw(v, clu_offset(v, first), e, sizeof(e), true);
        if(err){
            clu_free_chain(v, first);
            return err;
        }
    }
    uint8_t raw[ENT_SIZE];
    memset(raw, 0, sizeof(raw));
    raw[11] = is_dir ? ATTR_DIR : ATTR_ARCHIVE;
    stamp_times(raw, now);
    put16(raw + 20, (uint16_t)(first >> 16));
    put16(raw + 26, (uint16_t)first);
    uint32_t lfn_off, ent_off;
    err = dir_add(v, d->first, nm, nn, raw, lfn_off, ent_off);
    if(err){
        if(first){
            clu_free_chain(v, first);
        }
        return err;
    }
    d->rd_valid = false;
    Node* n = node_for(v, d->first, raw, ent_off, lfn_off);
    if(!n){
        dir_delete(v, d->first, lfn_off, ent_off);
        if(first){
            clu_free_chain(v, first);
        }
        return -ENOMEM;
    }
    out = n->ino;
    return 0;
}

int fat_create(Inode* dir, const char* name, uint32_t len, uint32_t, Inode*& out){
    Node* d = node_of(dir);
    MutexGuard g(d->vol->lock);
    return make_entry(d, name, len, false, out);
}

int fat_mkdir(Inode* dir, const char* name, uint32_t len, uint32_t, Inode*& out){
    Node* d = node_of(dir);
    MutexGuard g(d->vol->lock);
    return make_entry(d, name, len, true, out);
}

int fat_remove(Inode* dir, Inode* victim, bool is_dir){
    Node* d = node_of(dir);
    Node* n = node_of(victim);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    if(v->readonly){
        return -EROFS;
    }
    if(n->dead || n->dir_first != d->first){
        return -ENOENT;
    }
    if(is_dir){
        int rc = dir_empty(v, n->first);
        if(rc <= 0){
            return rc < 0 ? rc : -ENOTEMPTY;
        }
    }
    int err = vol_begin_write(v);
    if(!err){
        err = dir_delete(v, d->first, n->lfn_off, n->ent_off);
    }
    if(err){
        return err;
    }
    n->dead = true;
    victim->nlink = 0;
    d->rd_valid = false;
    dir->mtime = dir->ctime = vfs_now();
    return 0;
}

int fat_unlink(Inode* dir, const char*, uint32_t, Inode* victim){
    return fat_remove(dir, victim, false);
}

int fat_rmdir(Inode* dir, const char*, uint32_t, Inode* victim){
    return fat_remove(dir, victim, true);
}

int fat_rename(Inode* odir, const char*, uint32_t, Inode* ndir, const char* nname, uint32_t nlen, Inode* src,
               Inode* victim){
    Node* od = node_of(odir);
    Node* nd = node_of(ndir);
    Node* sn = node_of(src);
    Node* vn = victim ? node_of(victim) : nullptr;
    Volume* v = od->vol;
    MutexGuard g(v->lock);
    if(v->readonly){
        return -EROFS;
    }
    uint16_t nm[NAME16_MAX];
    uint32_t nn;
    int err = name_in(nname, nlen, nm, nn);
    if(err){
        return err;
    }
    if(sn->dead || sn->dir_first != od->first || (vn && vn->dead)){
        return -ENOENT;
    }
    if(vn && S_ISDIR(vn->ino->mode)){
        int rc = dir_empty(v, vn->first);
        if(rc <= 0){
            return rc < 0 ? rc : -ENOTEMPTY;
        }
    }
    err = vol_begin_write(v);
    if(err){
        return err;
    }
    uint8_t raw[ENT_SIZE];
    DirIter it;
    it_init(it, v, sn->dir_first);
    int rc = dir_read_raw(it, sn->ent_off, raw);
    if(rc){
        return rc < 0 ? rc : -EIO;
    }
    uint8_t saved_s[22] = {};
    uint8_t saved_v[22] = {};
    err = dir_save_marks(v, sn->dir_first, sn->lfn_off, sn->ent_off, saved_s);
    if(!err && vn){
        err = dir_save_marks(v, vn->dir_first, vn->lfn_off, vn->ent_off, saved_v);
    }
    if(!err){
        err = dir_delete(v, sn->dir_first, sn->lfn_off, sn->ent_off);
    }
    if(!err && vn){
        err = dir_delete(v, vn->dir_first, vn->lfn_off, vn->ent_off);
    }
    uint32_t lfn_off = 0;
    uint32_t ent_off = 0;
    if(!err){
        err = dir_add(v, nd->first, nm, nn, raw, lfn_off, ent_off);
    }
    if(err){
        dir_undelete(v, sn->dir_first, sn->lfn_off, sn->ent_off, saved_s);
        if(vn){
            dir_undelete(v, vn->dir_first, vn->lfn_off, vn->ent_off, saved_v);
        }
        return err;
    }
    sn->dir_first = nd->first;
    sn->ent_off = ent_off;
    sn->lfn_off = lfn_off;
    if(vn){
        vn->dead = true;
        victim->nlink = 0;
    }
    if(S_ISDIR(src->mode) && od != nd){
        DirIter di;
        it_init(di, v, sn->first);
        uint8_t dd[ENT_SIZE];
        if(dir_read_raw(di, ENT_SIZE, dd) == 0){
            uint32_t c = nd == v->root ? 0 : nd->first;
            put16(dd + 20, (uint16_t)(c >> 16));
            put16(dd + 26, (uint16_t)c);
            dir_write_raw(di, ENT_SIZE, dd);
        }
    }
    od->rd_valid = false;
    nd->rd_valid = false;
    src->ctime = vfs_now();
    return 0;
}

int64_t fat_read(Inode* i, uint64_t pos, void* buf, uint64_t len){
    Node* n = node_of(i);
    Volume* v = n->vol;
    MutexGuard g(v->lock);
    uint64_t size = i->size;
    if(pos >= size){
        return 0;
    }
    if(len > size - pos){
        len = size - pos;
    }
    uint64_t done = 0;
    while(done < len){
        uint64_t cur = pos + done;
        uint32_t co = (uint32_t)(cur % v->cluster_bytes);
        uint64_t chunk = v->cluster_bytes - co;
        if(chunk > len - done){
            chunk = len - done;
        }
        uint32_t c;
        int rc = node_cluster(n, (uint32_t)(cur / v->cluster_bytes), false, c);
        int err = rc > 0 ? -EIO : rc;
        if(!err){
            err = vol_rw(v, clu_offset(v, c) + co, (uint8_t*)buf + done, chunk, false);
        }
        if(err){
            return done ? (int64_t)done : err;
        }
        done += chunk;
    }
    return (int64_t)done;
}

int64_t fat_write(Inode* i, uint64_t pos, const void* buf, uint64_t len){
    if(len == 0){
        return 0;
    }
    Node* n = node_of(i);
    Volume* v = n->vol;
    MutexGuard g(v->lock);
    if(v->readonly){
        return -EROFS;
    }
    const uint64_t MAX_SIZE = 0xFFFFFFFFull;
    if(pos >= MAX_SIZE){
        return -EFBIG;
    }
    if(len > MAX_SIZE - pos){
        len = MAX_SIZE - pos;
    }
    int err = vol_begin_write(v);
    if(err){
        return err;
    }
    uint64_t size = i->size;
    uint32_t cb = v->cluster_bytes;
    // Scriere dupa sfarsit: ce a ramas in ultimul cluster peste size trebuie sa se citeasca zero
    if(pos > size && size % cb){
        uint64_t stop = (size / cb + 1) * (uint64_t)cb;
        err = node_zero(n, size, pos < stop ? pos : stop);
        if(err){
            return err;
        }
    }
    uint64_t done = 0;
    while(done < len){
        uint64_t cur = pos + done;
        uint32_t co = (uint32_t)(cur % cb);
        uint64_t chunk = cb - co;
        if(chunk > len - done){
            chunk = len - done;
        }
        uint32_t c;
        err = node_cluster(n, (uint32_t)(cur / cb), true, c);
        if(!err){
            err = vol_rw(v, clu_offset(v, c) + co, (void*)((const uint8_t*)buf + done), chunk, true);
        }
        if(err){
            break;
        }
        done += chunk;
    }
    uint64_t end = pos + done;
    if(end > size){
        __atomic_store_n(&i->size, end, __ATOMIC_RELAXED);
    } else if(done == 0){
        node_trim(n, size);                     // clusterele adaugate pentru o scriere care a esuat
    }
    if(done){
        i->mtime = i->ctime = vfs_now();
    }
    int ferr = node_flush(n);
    if(done == 0){
        return err;
    }
    return ferr && !err ? ferr : (int64_t)done;
}

int fat_truncate(Inode* i, uint64_t size){
    Node* n = node_of(i);
    Volume* v = n->vol;
    MutexGuard g(v->lock);
    if(v->readonly){
        return -EROFS;
    }
    if(size > 0xFFFFFFFFull){
        return -EFBIG;
    }
    int err = vol_begin_write(v);
    if(err){
        return err;
    }
    uint64_t old = i->size;
    uint32_t cb = v->cluster_bytes;
    if(size > old){
        if(old % cb){
            uint64_t stop = (old / cb + 1) * (uint64_t)cb;
            err = node_zero(n, old, size < stop ? size : stop);
        }
        uint32_t c;
        if(!err){
            err = node_cluster(n, (uint32_t)((size - 1) / cb), true, c);
        }
        if(err){
            node_trim(n, old);
            return err;
        }
    } else if(size < old){
        err = node_trim(n, size);
        if(err){
            return err;
        }
    }
    __atomic_store_n(&i->size, size, __ATOMIC_RELAXED);
    i->mtime = i->ctime = vfs_now();
    return node_flush(n);
}

int fat_readdir(Inode* dir, uint64_t index, DirEntry& out){
    Node* d = node_of(dir);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    DirIter it;
    it_init(it, v, d->first);
    uint64_t vis = 0;
    uint32_t off = 0;
    if(d->rd_valid && index >= d->rd_vis){
        vis = d->rd_vis;
        off = d->rd_off;
    }
    for(;;){
        Entry e;
        int rc = dir_next(it, off, e);
        if(rc <= 0){
            return rc;
        }
        if(e.raw[0] == '.' && !e.has_lfn){
            continue;                           // "." si ".."
        }
        uint32_t len = utf16_to_utf8(e.name, e.nlen, out.name, NAME_MAX);
        if(len == 0xFFFFFFFFu || len == 0){
            continue;                           // nume pe care VFS-ul nu il poate reprezenta
        }
        if(vis == index){
            out.name[len] = 0;
            out.name_len = (uint8_t)len;
            out.type = (e.raw[11] & ATTR_DIR) ? DT_DIR : DT_REG;
            uint32_t first = (uint32_t)le16(e.raw + 20) << 16 | le16(e.raw + 26);
            out.ino = synth_ino(first, d->first, e.ent_off);
            d->rd_valid = true;
            d->rd_vis = vis + 1;
            d->rd_off = off;
            return 1;
        }
        vis++;
    }
}

const InodeOps fat_ops = {fat_lookup, fat_create, fat_mkdir, nullptr, fat_unlink, fat_rmdir, fat_read, fat_write,
                          fat_truncate, fat_readdir, nullptr, fat_rename};

// ---- sistemul de fisiere ----

void fat_release(Inode* i){
    Node* n = node_of(i);
    Volume* v = n->vol;
    if(n == v->root){
        return;                                 // ramane al volumului pana la kill
    }
    MutexGuard g(v->lock);
    if(i->nlink == 0 && !v->readonly){
        clu_free_chain(v, n->first);
    }
    node_unlink(v, n);
    inode_free(i);
    kfree(n);
}

void volume_free(Volume* v){
    for(uint32_t k = 0; k < FAT_WINDOWS; k++){
        kfree(v->win[k].data);
    }
    kfree(v);
}

void fat_kill(SuperBlock* sb){
    Volume* v = (Volume*)sb->priv;
    {
        MutexGuard g(v->lock);
        int err = vol_flush_state(v);
        if(err){
            kprintf("fat32: scrierea starii volumului a esuat (%d)\n", err);
        }
    }
    int err = bcache().invalidate(v->dev);
    if(err){
        kprintf("fat32: sync pe %s a esuat (%d)\n", "volum", err);
    }
    for(Volume** pp = &volumes; *pp; pp = &(*pp)->next){
        if(*pp == v){
            *pp = v->next;
            break;
        }
    }
    if(v->root){
        node_unlink(v, v->root);
        inode_free(v->root->ino);
        kfree(v->root);
    }
    volume_free(v);
}

const SuperBlockOps fat_sb_ops = {fat_release, fat_kill};

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

int count_free(Volume* v){
    uint32_t free_n = 0;
    for(uint32_t c = 2; c < v->clusters + 2; c++){
        uint32_t val;
        int err = fat_get(v, c, val);
        if(err){
            return err;
        }
        free_n += val == 0;
    }
    v->free_count = free_n;
    return 0;
}

int fat_mount(FileSystemType*, SuperBlock* sb, const char* dev_name, const char* data){
    BlockDevice* dev = find_block(dev_name);
    if(!dev){
        return -ENODEV;
    }
    for(Volume* x = volumes; x; x = x->next){
        if(x->dev == dev){
            return -EBUSY;
        }
    }
    bool ro = sb->flags & MS_RDONLY;
    while(data && *data){
        if(data[0] == 'r' && data[1] == 'o' && (data[2] == ',' || !data[2])){
            ro = true;
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
    uint8_t sec[512];
    Buf* b = bread(dev, 0);
    if(!b){
        return -EIO;
    }
    memcpy(sec, b->data, 512);
    brelse(b);
    Bpb bpb;
    if(!bpb_parse(sec, bpb)){
        return -EINVAL;
    }
    uint64_t dev_bytes = dev->block_count() * (uint64_t)dev->block_size();
    uint64_t fat_total = (uint64_t)bpb.nfats * bpb.fat_sectors;
    if((uint64_t)bpb.total * bpb.bps > dev_bytes || (uint64_t)bpb.reserved + fat_total >= bpb.total){
        return -EINVAL;
    }
    uint64_t data_sectors = bpb.total - bpb.reserved - fat_total;
    uint64_t clusters = data_sectors / bpb.spc;
    if(clusters < 1 || clusters > 0x0FFFFFF0ull || bpb.root_cluster >= clusters + 2 ||
       (uint64_t)(clusters + 2) * 4 > (uint64_t)bpb.fat_sectors * bpb.bps){
        return -EINVAL;
    }

    void* mem = kzalloc(sizeof(Volume));
    if(!mem){
        return -ENOMEM;
    }
    Volume* v = new (mem) Volume();
    v->dev = dev;
    v->sb = sb;
    v->readonly = ro;
    v->bps = bpb.bps;
    v->spc = bpb.spc;
    v->cluster_bytes = bpb.bps * bpb.spc;
    v->nfats = bpb.nfats;
    v->mirror = !(bpb.ext_flags & 0x80);
    v->active_fat = v->mirror ? 0 : (bpb.ext_flags & 15);
    if(v->active_fat >= v->nfats){
        v->active_fat = 0;
    }
    v->fat_off = (uint64_t)bpb.reserved * bpb.bps;
    v->fat_bytes = (uint64_t)bpb.fat_sectors * bpb.bps;
    v->data_off = v->fat_off + fat_total * bpb.bps;
    v->clusters = (uint32_t)clusters;
    v->root_cluster = bpb.root_cluster;
    v->free_count = 0xFFFFFFFFu;
    v->next_free = 2;
    for(uint32_t k = 0; k < FAT_WINDOWS; k++){
        v->win[k].data = (uint8_t*)kmalloc(FAT_WINDOW);
        if(!v->win[k].data){
            volume_free(v);
            return -ENOMEM;
        }
    }
    sb->priv = v;
    sb->ops = &fat_sb_ops;
    sb->flags |= SB_NOCACHE | (ro ? MS_RDONLY : 0u);

    int err = 0;
    uint32_t f1 = 0;
    if(bpb.fsinfo > 0 && bpb.fsinfo < bpb.reserved){
        uint8_t fi[512];
        err = vol_rw(v, (uint64_t)bpb.fsinfo * bpb.bps, fi, 512, false);
        if(!err && le32(fi) == 0x41615252 && le32(fi + 484) == 0x61417272 && le32(fi + 508) == 0xAA550000){
            v->fsinfo_sector = bpb.fsinfo;
            uint32_t fc = le32(fi + 488);
            uint32_t nf = le32(fi + 492);
            if(fc != 0xFFFFFFFFu && fc != 0 && fc <= v->clusters){
                v->free_count = fc;
            }
            if(nf >= 2 && nf < v->clusters + 2){
                v->next_free = nf;
            }
        }
    }
    if(!err){
        err = fat_get(v, 1, f1);
    }
    if(!err && v->free_count == 0xFFFFFFFFu){
        err = count_free(v);
    }
    if(err){
        volume_free(v);
        return err;
    }
    v->was_dirty = !(f1 & FAT_CLEAN) || (sec[0x41] & 1);

    void* rm = kzalloc(sizeof(Node));
    Inode* ri = rm ? inode_alloc(sb, v->root_cluster, S_IFDIR | 0755, &fat_ops) : nullptr;
    if(!ri){
        kfree(rm);
        volume_free(v);
        return -ENOMEM;
    }
    Node* root = new (rm) Node();
    root->vol = v;
    root->ino = ri;
    root->first = v->root_cluster;
    ri->priv = root;
    ri->nlink = 2;
    v->root = root;
    node_link(v, root);
    sb->root_inode = ri;
    v->next = volumes;
    volumes = v;
    return 0;
}

FileSystemType fat32_type = {"fat32", fat_mount, nullptr};

}

void fat32_init(){
    fs_register(&fat32_type);
}

int fat32_automount() {
    for(int k = 0; k < hal_count(HalKind::Block); k++){
        BlockDevice* dev = hal_block(k);
        uint8_t sec[512];
        Bpb bpb;
        if(!dev || block_read_bytes(dev, 0, sec, 512) || !bpb_parse(sec, bpb)){
            continue;
        }
        const char* name = hal_name(HalKind::Block, k);
        Path none = {};
        int err = vfs_mount(none, name, "/media", "fat32", 0, nullptr);
        if(err){
            kprintf("fat32: montarea lui %s pe /media a esuat (%d)\n", name, err);
            continue;
        }
        kprintf("[OK] fat32 montat pe /media (%s)\n", name);
        return 0;
    }
    return -ENODEV;
}

uint32_t fat32_free_clusters(const Inode* ino){
    Volume* v = vol_of(ino);
    MutexGuard g(v->lock);
    return v->free_count;
}
