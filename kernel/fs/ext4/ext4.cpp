#include "fs/ext4/ext4.h"
#include "fs/ext4/ext4_priv.h"
#include "fs/vfs/vfs.h"
#include "hal/block.h"
#include "hal/hal.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/new.h"
#include "lib/kprintf.h"
#include "lib/crc.h"
#include "time/clock.h"
#include "uapi/syscall_nr.h"

using namespace ext4;

namespace {

extern const InodeOps ext4_ops;
extern const SuperBlockOps ext4_sb_ops;

const uint32_t COMMIT_INTERVAL_MS = 5000;

Node* node_of(const Inode* i){
    return (Node*)i->priv;
}

#define TRY(x) do { int rc_ = (x); if(rc_){ return rc_; } } while(0)

uint64_t min64(uint64_t a, uint64_t b){
    return a < b ? a : b;
}

// ---- operatii pe inode-uri (citire) ----

int e4_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    Node* d = node_of(dir);
    Volume* v = d->vol;
    if(!S_ISDIR(dir->mode)){
        return -ENOTDIR;
    }
    MutexGuard g(v->lock);
    uint32_t ino;
    int err = dir_lookup(d, name, len, ino);
    if(err){
        return err;
    }
    Node* n;
    err = node_load(v, ino, n);
    if(err){
        return err == -ENOENT ? -EIO : err;
    }
    out = n->ino;
    return 0;
}

int64_t e4_read(Inode* ino, uint64_t pos, void* buf, uint64_t len){
    if(S_ISDIR(ino->mode)){
        return -EISDIR;
    }
    if(!S_ISREG(ino->mode)){
        return -EINVAL;
    }
    Node* n = node_of(ino);
    if(n->vol->rw){
        MutexGuard g(n->vol->lock);             // scrierile schimba arborele de extente
        return node_read(n, pos, buf, len);
    }
    return node_read(n, pos, buf, len);
}

int e4_readdir(Inode* dir, uint64_t index, DirEntry& out){
    Node* d = node_of(dir);
    if(!S_ISDIR(dir->mode)){
        return -ENOTDIR;
    }
    MutexGuard g(d->vol->lock);
    return dir_readdir(d, index, out);
}

int64_t e4_readlink(Inode* ino, char* buf, uint64_t len){
    Node* n = node_of(ino);
    if(!S_ISLNK(ino->mode)){
        return -EINVAL;
    }
    uint64_t take = ino->size < len ? ino->size : len;
    if(n->fast_symlink){
        memcpy(buf, n->blk, take);
        return (int64_t)take;
    }
    if(ino->size > n->vol->bs){
        return -EIO;
    }
    MutexGuard g(n->vol->lock);
    return node_read(n, 0, buf, take);
}

// ---- scriere ----

void drop_node(Volume* v, Node* n){
    for(Node** pp = &v->hash[n->ino->ino % NODE_BUCKETS]; *pp; pp = &(*pp)->next){
        if(*pp == n){
            *pp = n->next;
            break;
        }
    }
    inode_free(n->ino);
    kfree(n);
}

int touch(Node* n){
    n->ino->mtime = n->ino->ctime = vfs_now();
    return inode_store(n);
}

uint32_t group_of(Volume* v, const Inode* i){
    return (uint32_t)((i->ino - 1) / v->ipg);
}

int check_name(Volume* v, Node* d, const char* name, uint32_t len){
    if(!S_ISDIR(d->ino->mode)){
        return -ENOTDIR;
    }
    if(len == 0 || len > 255){
        return -ENAMETOOLONG;
    }
    uint32_t ino;
    int rc = dir_lookup(d, name, len, ino);
    (void)v;
    if(rc == 0){
        return -EEXIST;
    }
    return rc == -ENOENT ? 0 : rc;
}

// Adaugare in lista inodurilor orfane (sb.s_last_orphan, i_dtime = urmatorul)
int orphan_add_(Node* n){
    Volume* v = n->vol;
    n->dtime = le32(v->sbraw + 0xE8);
    put32(v->sbraw + 0xE8, (uint32_t)n->ino->ino);
    v->sb_dirty = true;
    return inode_store(n);
}

int free_acl(Node* n){
    Volume* v = n->vol;
    if(!n->file_acl){
        return 0;
    }
    uint64_t blk = n->file_acl;
    n->file_acl = 0;
    n->blocks512 -= v->bs >> 9;
    uint8_t* b = tx_get(v, blk, true);
    if(!b){
        return -ENOSPC;
    }
    uint32_t refs = le32(b + 4);
    if(le32(b) == 0xEA020000u && refs > 1){
        put32(b + 4, refs - 1);                 // bloc partajat: doar scadem contorul (fara checksum recalculat)
        return 0;
    }
    return bfree(v, blk, 1, true);
}

int e4_create(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out){
    Node* d = node_of(dir);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    TRY(tx_begin(v, 40));
    TRY(check_name(v, d, name, len));
    uint32_t ino;
    TRY(ialloc(v, group_of(v, dir), false, ino));
    Node* n;
    int rc = inode_init(v, ino, mode, INODE_EXTENTS_FL, n);
    if(rc){
        ifree(v, ino, false);
        return rc;
    }
    rc = dir_add(d, name, len, ino, ftype_of_mode(mode));
    if(rc){
        ifree(v, ino, false);
        drop_node(v, n);
        return rc;
    }
    TRY(touch(d));
    out = n->ino;
    return 0;
}

int e4_mkdir(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out){
    Node* d = node_of(dir);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    TRY(tx_begin(v, 48));
    TRY(check_name(v, d, name, len));
    if(dir->nlink >= 65000){
        return -EMLINK;
    }
    uint32_t ino;
    TRY(ialloc(v, group_of(v, dir), true, ino));
    Node* n;
    int rc = inode_init(v, ino, mode, INODE_EXTENTS_FL, n);
    if(rc){
        ifree(v, ino, true);
        return rc;
    }
    uint64_t first;
    uint32_t got;
    rc = file_alloc_range(n, 0, 1, first, got);
    uint8_t* blk = rc ? nullptr : tx_get(v, first, false);
    if(!rc && !blk){
        rc = -ENOSPC;
    }
    if(!rc){
        dir_init_block(n, blk, ino, (uint32_t)dir->ino);
        n->ino->size = v->bs;
        n->ino->nlink = 2;
        rc = inode_store(n);
    }
    if(!rc){
        rc = dir_add(d, name, len, ino, 2);
    }
    if(rc){
        ifree(v, ino, true);
        drop_node(v, n);
        return rc;
    }
    dir->nlink++;
    TRY(touch(d));
    out = n->ino;
    return 0;
}

int e4_symlink(Inode* dir, const char* name, uint32_t len, const char* target, Inode*& out){
    Node* d = node_of(dir);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    uint32_t tl = 0;
    while(target[tl]){
        tl++;
    }
    if(tl == 0 || tl >= v->bs){
        return tl ? -ENAMETOOLONG : -EINVAL;
    }
    TRY(tx_begin(v, 40));
    TRY(check_name(v, d, name, len));
    uint32_t ino;
    TRY(ialloc(v, group_of(v, dir), false, ino));
    bool fast = tl < 60;
    Node* n;
    int rc = inode_init(v, ino, S_IFLNK | 0777, fast ? 0 : INODE_EXTENTS_FL, n);
    if(rc){
        ifree(v, ino, false);
        return rc;
    }
    n->fast_symlink = fast;
    if(fast){
        memset(n->blk, 0, 60);
        memcpy(n->blk, target, tl);
    } else {
        uint64_t first;
        uint32_t got;
        rc = file_alloc_range(n, 0, 1, first, got);
        uint8_t* blk = rc ? nullptr : tx_get(v, first, false);
        if(!rc && !blk){
            rc = -ENOSPC;
        }
        if(!rc){
            memcpy(blk, target, tl);
        }
    }
    if(!rc){
        n->ino->size = tl;
        rc = inode_store(n);
    }
    if(!rc){
        rc = dir_add(d, name, len, ino, 7);
    }
    if(rc){
        ifree(v, ino, false);
        drop_node(v, n);
        return rc;
    }
    TRY(touch(d));
    out = n->ino;
    return 0;
}

int e4_unlink(Inode* dir, const char* name, uint32_t len, Inode* victim){
    Node* d = node_of(dir);
    Node* vn = node_of(victim);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    TRY(tx_begin(v, 24));
    TRY(dir_remove(d, name, len));
    victim->ctime = vfs_now();
    if(victim->nlink > 0){
        victim->nlink--;
    }
    if(victim->nlink == 0){
        TRY(orphan_add_(vn));
    } else {
        TRY(inode_store(vn));
    }
    return touch(d);
}

int e4_rmdir(Inode* dir, const char* name, uint32_t len, Inode* victim){
    Node* d = node_of(dir);
    Node* vn = node_of(victim);
    Volume* v = d->vol;
    MutexGuard g(v->lock);
    TRY(tx_begin(v, 24));
    int err;
    if(!dir_is_empty(vn, err)){
        return err ? err : -ENOTEMPTY;
    }
    TRY(dir_remove(d, name, len));
    victim->nlink = 0;
    victim->ctime = vfs_now();
    if(dir->nlink > 2){
        dir->nlink--;
    }
    TRY(orphan_add_(vn));
    return touch(d);
}

// Zero coada ultimului bloc (de la marimea veche pana la capatul blocului) inainte ca fisierul sa creasca. Trunchierea
// nu scrie date pe loc (o cadere inainte de commit ar arata zerourile sub marimea veche); octetii de dupa EOF raman
// vechi pana aici, unde sunt inca invizibili: daca nu apuca sa fie comisa marimea noua, nimeni nu-i vede.
static int zero_tail(Node* n, uint64_t old_size){
    Volume* v = n->vol;
    uint32_t boff = (uint32_t)(old_size & (v->bs - 1));
    if(!boff){
        return 0;
    }
    uint64_t phys, run;
    TRY(map_block(n, (uint32_t)(old_size >> v->bs_shift), 1, phys, run));
    if(!phys){
        return 0;
    }
    Buf* b = bread(v->dev, phys);
    if(!b){
        return -EIO;
    }
    memset(b->data + boff, 0, v->bs - boff);
    bdirty(b);
    brelse(b);
    return 0;
}

int64_t e4_write(Inode* ino, uint64_t pos, const void* buf, uint64_t len){
    Node* n = node_of(ino);
    Volume* v = n->vol;
    if(!S_ISREG(ino->mode)){
        return S_ISDIR(ino->mode) ? -EISDIR : -EINVAL;
    }
    MutexGuard g(v->lock);
    if(!v->rw){
        return -EROFS;
    }
    if(!(n->flags & INODE_EXTENTS_FL) || (n->flags & INODE_HUGE_FILE_FL)){
        return -EOPNOTSUPP;
    }
    if(len == 0){
        return 0;
    }
    if(pos + len < pos || pos + len > (1ull << 44)){
        return -EFBIG;
    }
    if(pos + len > ino->size){
        TRY(zero_tail(n, ino->size));
    }
    const uint8_t* src = (const uint8_t*)buf;
    uint64_t done = 0;
    int err = 0;
    while(done < len){
        if((err = tx_begin(v, 40))){
            break;
        }
        uint64_t p = pos + done;
        uint32_t lblk = (uint32_t)(p >> v->bs_shift);
        uint64_t last = (pos + len - 1) >> v->bs_shift;
        uint64_t phys, run;
        err = map_block(n, lblk, last - lblk + 1, phys, run);
        if(err){
            break;
        }
        bool fresh = false;
        if(!phys){
            uint32_t got;
            err = file_alloc_range(n, lblk, (uint32_t)run, phys, got);
            if(err){
                break;
            }
            run = got;
            fresh = true;
        }
        uint32_t boff = (uint32_t)(p & (v->bs - 1));
        for(uint64_t k = 0; k < run && done < len; k++){
            uint32_t take = (uint32_t)min64(v->bs - boff, len - done);
            bool full = boff == 0 && take == v->bs;
            Buf* b = (full || fresh) ? bget(v->dev, phys + k) : bread(v->dev, phys + k);
            if(!b){
                err = -EIO;
                break;
            }
            if(!full && fresh){
                memset(b->data, 0, v->bs);
            }
            memcpy(b->data + boff, src + done, take);
            bdirty(b);
            brelse(b);
            done += take;
            boff = 0;
        }
        if(err){
            break;
        }
        if(pos + done > ino->size){
            ino->size = pos + done;
        }
        ino->mtime = ino->ctime = vfs_now();
        if((err = inode_store(n))){
            break;
        }
    }
    if(done){
        return (int64_t)done;
    }
    return err;
}

// Termina trunchierea (inode pe lista de orfani): elibereaza blocurile de la `from`, pas cu pas
int shrink_blocks(Node* n, uint32_t from){
    Volume* v = n->vol;
    for(;;){
        TRY(tx_begin(v, 40));
        bool more;
        TRY(ext_truncate(n, from, 8, more));
        if(!more){
            return 0;
        }
    }
}

int e4_truncate(Inode* ino, uint64_t size){
    Node* n = node_of(ino);
    Volume* v = n->vol;
    if(!S_ISREG(ino->mode)){
        return -EINVAL;
    }
    MutexGuard g(v->lock);
    if(!v->rw){
        return -EROFS;
    }
    if(!(n->flags & INODE_EXTENTS_FL) || (n->flags & INODE_HUGE_FILE_FL)){
        if(size == 0 && ino->size == 0){
            return 0;
        }
        return -EOPNOTSUPP;
    }
    if(size > (1ull << 44)){
        return -EFBIG;
    }
    TRY(tx_begin(v, 48));
    if(size >= ino->size){
        TRY(zero_tail(n, ino->size));
        ino->size = size;
        return touch(n);
    }
    ino->size = size;
    ino->mtime = ino->ctime = vfs_now();
    TRY(inode_store(n));
    uint32_t from = (uint32_t)((size + v->bs - 1) >> v->bs_shift);
    bool more;
    TRY(ext_truncate(n, from, 8, more));
    if(more){
        TRY(orphan_add_(n));
        TRY(shrink_blocks(n, from));
        TRY(tx_begin(v, 16));
        TRY(orphan_del(n));
        n->dtime = 0;
        TRY(inode_store(n));
    }
    return 0;
}

int e4_rename(Inode* odir, const char* oname, uint32_t olen, Inode* ndir, const char* nname, uint32_t nlen,
              Inode* src, Inode* victim){
    Node* od = node_of(odir);
    Node* nd = node_of(ndir);
    Node* sn = node_of(src);
    Volume* v = od->vol;
    MutexGuard g(v->lock);
    TRY(tx_begin(v, 56));
    if(nlen == 0 || nlen > 255){
        return -ENAMETOOLONG;
    }
    bool sdir = S_ISDIR(src->mode);
    if(sdir && odir != ndir && ndir->nlink >= 65000 && !victim){
        return -EMLINK;
    }
    uint8_t ft = ftype_of_mode(src->mode);
    Timespec now = vfs_now();
    if(victim){
        Node* vn = node_of(victim);
        if(S_ISDIR(victim->mode)){
            int err;
            if(!dir_is_empty(vn, err)){
                return err ? err : -ENOTEMPTY;
            }
        }
        TRY(dir_set_ino(nd, nname, nlen, (uint32_t)src->ino, ft));
        victim->ctime = now;
        if(S_ISDIR(victim->mode)){
            victim->nlink = 0;
            if(ndir->nlink > 2){
                ndir->nlink--;
            }
        } else if(victim->nlink > 0){
            victim->nlink--;
        }
        if(victim->nlink == 0){
            TRY(orphan_add_(vn));
        } else {
            TRY(inode_store(vn));
        }
    } else {
        TRY(dir_add(nd, nname, nlen, (uint32_t)src->ino, ft));
    }
    TRY(dir_remove(od, oname, olen));
    if(sdir && odir != ndir){
        TRY(dir_set_dotdot(sn, (uint32_t)ndir->ino));
        if(odir->nlink > 2){
            odir->nlink--;
        }
        ndir->nlink++;
    }
    src->ctime = now;
    TRY(inode_store(sn));
    TRY(touch(od));
    if(nd != od){
        TRY(touch(nd));
    }
    return 0;
}

const InodeOps ext4_ops = {e4_lookup, e4_create, e4_mkdir, e4_symlink, e4_unlink, e4_rmdir, e4_read, e4_write,
                           e4_truncate, e4_readdir, e4_readlink, e4_rename};

// ---- sistemul de fisiere ----

void node_unlink(Volume* v, Node* n){
    for(Node** pp = &v->hash[n->ino->ino % NODE_BUCKETS]; *pp; pp = &(*pp)->next){
        if(*pp == n){
            *pp = n->next;
            return;
        }
    }
}

void e4_release(Inode* i){
    Node* n = node_of(i);
    Volume* v = n->vol;
    if(i->nlink != 0 || n == v->root){
        return;                                 // inode-urile raman ale volumului pana la kill
    }
    MutexGuard g(v->lock);
    if(v->rw){
        int rc = node_destroy(n);
        if(rc){
            kprintf("ext4: eliberarea inode-ului %lu a esuat (%d); ramane orfan\n", i->ino, rc);
            return;                             // ramane in lista de orfani, curatat la urmatorul mount
        }
    }
    node_unlink(v, n);
    inode_free(i);
    kfree(n);
}

void sync_hook(void* arg, bool periodic){
    Volume* v = (Volume*)arg;
    MutexGuard g(v->lock);
    if(!v->rw || (!jbd2_dirty(v->jnl) && !v->sb_dirty)){
        return;
    }
    if(periodic && uptime_ms() - v->last_commit_ms < v->commit_ms){
        return;
    }
    tx_commit(v);
}

int sb_write_direct(Volume* v){
    sb_csum_update(v, v->sbraw);
    Buf* b = bread(v->dev, 0);
    if(!b){
        return -EIO;
    }
    memcpy(b->data + 1024, v->sbraw, 1024);
    bdirty(b);
    brelse(b);
    return bsync(v->dev);
}

void volume_free(Volume* v){
    for(uint32_t b = 0; b < NODE_BUCKETS; b++){
        Node* n = v->hash[b];
        while(n){
            Node* next = n->next;
            inode_free(n->ino);
            kfree(n);
            n = next;
        }
        v->hash[b] = nullptr;
    }
    jbd2_free(v->jnl);
    freed_release(v);
    kfree(v->freed_bm);
    kfree(v->freed_list);
    kfree(v->gd_off);
    kfree(v->itable);
    kfree(v);
}

void e4_kill(SuperBlock* sb){
    Volume* v = (Volume*)sb->priv;
    if(v->rw){
        bcache_del_hook(v);
        MutexGuard g(v->lock);
        int rc = tx_commit(v);
        if(!rc && v->rw){
            put32(v->sbraw + 0x60, le32(v->sbraw + 0x60) & ~INCOMPAT_RECOVER);
            put16(v->sbraw + 0x3A, le16(v->sbraw + 0x3A) | STATE_VALID);
            rc = sb_write_direct(v);
        }
        if(rc){
            kprintf("ext4: oprirea curata a esuat (%d), jurnalul ramane de reluat\n", rc);
        }
    }
    int err = bcache().invalidate(v->dev);
    if(err){
        kprintf("ext4: invalidarea cache-ului a esuat (%d)\n", err);
    }
    volume_free(v);
}

const SuperBlockOps ext4_sb_ops = {e4_release, e4_kill};

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

bool pow2(uint32_t x){
    return x && !(x & (x - 1));
}

struct Opts {
    bool ro;
    bool rw;
    bool noload;
    uint32_t commit_s;
};

int parse_options(const char* data, Opts& o){
    while(data && *data){
        if(data[0] == 'r' && data[1] == 'o' && (data[2] == ',' || !data[2])){
            o.ro = true;
        } else if(data[0] == 'r' && data[1] == 'w' && (data[2] == ',' || !data[2])){
            o.rw = true;
        } else if(memcmp(data, "noload", 6) == 0 && (data[6] == ',' || !data[6])){
            o.noload = true;
        } else if(memcmp(data, "commit=", 7) == 0){
            uint32_t x = 0;
            const char* q = data + 7;
            if(*q < '0' || *q > '9'){
                return -EINVAL;
            }
            while(*q >= '0' && *q <= '9'){
                x = x * 10 + (uint32_t)(*q++ - '0');
            }
            if(*q && *q != ','){
                return -EINVAL;
            }
            o.commit_s = x;
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
    return 0;
}

// Motivul pentru care volumul nu se poate monta cu scriere (nullptr = se poate)
const char* rw_blocker(const Volume* v, const uint8_t* s){
    if(v->bs != 4096){
        return "scrierea cere blocuri de 4 KiB";
    }
    if(!(v->incompat & INCOMPAT_EXTENTS) || !(v->incompat & INCOMPAT_FILETYPE)){
        return "fara extents/filetype";
    }
    if(v->incompat & (INCOMPAT_META_BG | INCOMPAT_MMP)){
        return "meta_bg / mmp";
    }
    if(v->ro_compat & ~RO_COMPAT_WRITABLE){
        return "trasaturi RO_COMPAT necunoscute";
    }
    uint32_t compat = le32(s + 0x5C);
    if(!(compat & COMPAT_HAS_JOURNAL)){
        return "fara jurnal";
    }
    if(compat & (COMPAT_FAST_COMMIT | COMPAT_ORPHAN_FILE)){
        return "fast_commit / orphan_file";
    }
    if(le16(s + 0x3A) & 0x2){
        return "sistemul de fisiere are erori (e2fsck)";
    }
    if(le32(s + 0xE0) != JOURNAL_INO || le32(s + 0xE4) != 0){
        return "jurnal extern";
    }
    return nullptr;
}

int jmap(void* ctx, uint64_t jblk, uint64_t& phys){
    Volume* v = (Volume*)ctx;
    if((jblk << v->bs_shift) >= v->jnode->ino->size){
        return -EIO;
    }
    uint64_t run;
    int rc = map_block(v->jnode, jblk, 1, phys, run);
    return rc ? rc : (phys ? 0 : -EIO);
}

// Deschide jurnalul (fara a modifica discul). 0 sau -errno
int journal_open(Volume* v){
    Node* jn;
    int err = node_load(v, JOURNAL_INO, jn);
    if(err){
        return err;
    }
    if(!S_ISREG(jn->ino->mode) || !(jn->flags & INODE_EXTENTS_FL)){
        return -EINVAL;
    }
    v->jnode = jn;
    Jbd2Map m = {jmap, v};
    v->jnl = jbd2_open(v->dev, m, v->bs, err);
    return v->jnl ? 0 : err;
}

// Inode-uri orfane ramase dintr-o oprire brusca: stergeri neterminate si trunchieri neterminate
void orphan_cleanup(Volume* v){
    uint32_t count = 0;
    for(uint32_t guard = 0; guard < v->inodes; guard++){
        uint32_t ino = le32(v->sbraw + 0xE8);
        if(!ino){
            break;
        }
        Node* n;
        int rc = node_load(v, ino, n, true);
        if(rc){
            kprintf("ext4: inode orfan %u ilizibil (%d), lista se abandoneaza\n", ino, rc);
            put32(v->sbraw + 0xE8, 0);
            v->sb_dirty = true;
            break;
        }
        count++;
        if(n->ino->nlink == 0){
            if(node_destroy(n)){
                kprintf("ext4: inode orfan %u nu a putut fi eliberat\n", ino);
                put32(v->sbraw + 0xE8, 0);
                v->sb_dirty = true;
                break;
            }
            node_unlink(v, n);
            inode_free(n->ino);
            kfree(n);
        } else {
            uint32_t from = (uint32_t)((n->ino->size + v->bs - 1) >> v->bs_shift);
            if((n->flags & INODE_EXTENTS_FL) && shrink_blocks(n, from) == 0 && tx_begin(v, 16) == 0 &&
               orphan_del(n) == 0){
                n->dtime = 0;
                inode_store(n);
            } else {
                put32(v->sbraw + 0xE8, n->dtime);
                v->sb_dirty = true;
            }
        }
    }
    if(count){
        tx_commit(v);
        kprintf("ext4: %u inode-uri orfane curatate\n", count);
    }
}

int e4_mount(FileSystemType*, SuperBlock* sb, const char* dev_name, const char* data){
    BlockDevice* dev = find_block(dev_name);
    if(!dev){
        return -ENODEV;
    }
    Opts opts = {};
    int err = parse_options(data, opts);
    if(err){
        return err;
    }
    uint8_t s[1024];
    Buf* sbuf = bread(dev, 0);
    if(!sbuf){
        return -EIO;
    }
    memcpy(s, sbuf->data + 1024, sizeof(s));
    brelse(sbuf);
    if(le16(s + 0x38) != SB_MAGIC){
        return -EINVAL;
    }
    uint32_t rev = le32(s + 0x4C);
    uint32_t incompat = rev ? le32(s + 0x60) : 0;
    uint32_t ro_compat = rev ? le32(s + 0x64) : 0;
    uint32_t unsupported = incompat & ~INCOMPAT_SUPPORTED;
    if(unsupported){
        kprintf("ext4: flag-uri incompatibile nesuportate (%x)\n", unsupported);
        return -EINVAL;
    }
    if(ro_compat & RO_COMPAT_BIGALLOC){
        kprintf("ext4: bigalloc nesuportat\n");
        return -EINVAL;
    }
    bool want_rw = !(sb->flags & MS_RDONLY) && !opts.ro && !opts.noload;
    bool needs_recovery = (incompat & INCOMPAT_RECOVER) != 0;
    if(!want_rw && needs_recovery && !opts.noload){
        kprintf("ext4: jurnalul cere recuperare (monteaza cu noload pentru a ignora jurnalul)\n");
        return -EINVAL;
    }
    uint32_t log_bs = le32(s + 0x18);
    if(log_bs > 2){
        kprintf("ext4: blocuri mai mari de 4 KiB nesuportate\n");
        return -EINVAL;
    }
    uint32_t bs = 1024u << log_bs;
    uint32_t isz = rev ? le16(s + 0x58) : 128;
    uint32_t desc = 32;
    if(incompat & INCOMPAT_64BIT){
        desc = le16(s + 0xFE);
        if(desc < 64 || desc > 1024 || !pow2(desc)){
            return -EINVAL;
        }
    }
    uint64_t blocks = le32(s + 4);
    if(incompat & INCOMPAT_64BIT){
        blocks |= (uint64_t)le32(s + 0x150) << 32;
    }
    uint32_t first_data = le32(s + 0x14);
    uint32_t bpg = le32(s + 0x20);
    uint32_t ipg = le32(s + 0x28);
    if(!pow2(isz) || isz < 128 || isz > bs || bpg == 0 || bpg > bs * 8 || ipg == 0 || ipg > bs * 8 ||
       first_data != (bs == 1024 ? 1u : 0u) || blocks <= first_data || le32(s + 0) == 0){
        return -EINVAL;
    }
    if(blocks * bs > dev->block_count() * (uint64_t)dev->block_size()){
        kprintf("ext4: sistemul de fisiere (%lu blocuri) depaseste dispozitivul\n", blocks);
        return -EINVAL;
    }
    uint64_t groups64 = (blocks - first_data + bpg - 1) / bpg;
    if(groups64 > (1u << 20) || groups64 * ipg > 0xFFFFFFFFull){
        return -EINVAL;
    }
    uint32_t groups = (uint32_t)groups64;
    uint32_t dpb = bs / desc;

    void* mem = kzalloc(sizeof(Volume));
    if(!mem){
        return -ENOMEM;
    }
    Volume* v = new (mem) Volume();
    v->dev = dev;
    v->sb = sb;
    v->bs = bs;
    v->bs_shift = 10 + log_bs;
    v->blocks = blocks;
    v->first_data_block = first_data;
    v->bpg = bpg;
    v->ipg = ipg;
    v->inodes = le32(s);
    if((uint64_t)groups * ipg < v->inodes){
        v->inodes = groups * ipg;
    }
    v->inode_size = isz;
    v->groups = groups;
    v->desc_size = desc;
    v->incompat = incompat;
    v->ro_compat = ro_compat;
    v->hash_version = s[0xFC];
    v->hash_unsigned = le32(s + 0x160) & SBF_UNSIGNED_HASH;
    for(int k = 0; k < 4; k++){
        v->hash_seed[k] = le32(s + 0xEC + 4 * k);
    }
    memcpy(v->uuid, s + 0x68, 16);
    memcpy(v->sbraw, s, sizeof(s));
    v->csum = (ro_compat & RO_COMPAT_METADATA_CSUM) != 0;
    v->gdt_csum = !v->csum && (ro_compat & RO_COMPAT_GDT_CSUM);
    v->csum_seed = (incompat & INCOMPAT_CSUM_SEED) ? le32(s + 0x270) : crc32c(0xFFFFFFFFu, v->uuid, 16);
    v->first_ino = rev ? le32(s + 0x54) : 11;
    v->iops = &ext4_ops;
    v->itable = (uint64_t*)kzalloc((size_t)groups * sizeof(uint64_t));
    v->gd_off = (uint64_t*)kzalloc((size_t)groups * sizeof(uint64_t));
    if(!v->itable || !v->gd_off){
        volume_free(v);
        return -ENOMEM;
    }
    sb->priv = v;
    sb->ops = &ext4_sb_ops;
    sb->flags |= MS_RDONLY;

    uint32_t first_meta = (incompat & INCOMPAT_META_BG) ? le32(s + 0x104) : 0xFFFFFFFFu;
    for(uint32_t g = 0; g < groups && !err; g++){
        uint32_t nr = g / dpb;
        uint64_t blk;
        if(nr < first_meta){
            blk = first_data + nr + 1;
        } else {
            uint32_t bg = nr * dpb;
            blk = (uint64_t)first_data + (uint64_t)bg * bpg + (group_has_super(s, bg, ro_compat) ? 1 : 0);
        }
        uint8_t gd[64];
        uint32_t take = desc < sizeof(gd) ? desc : sizeof(gd);
        uint64_t goff = (blk << v->bs_shift) + (uint64_t)(g % dpb) * desc;
        err = blk >= blocks ? -EIO : read_bytes(v, goff, gd, take);
        if(err){
            break;
        }
        v->gd_off[g] = goff;
        uint64_t it = le32(gd + 8);
        if(desc >= 64){
            it |= (uint64_t)le32(gd + 0x28) << 32;
        }
        if(it == 0 || it + ((uint64_t)ipg * isz + bs - 1) / bs > blocks){
            err = -EINVAL;
            kprintf("ext4: tabela de inode-uri a grupului %u e in afara sistemului de fisiere\n", g);
            break;
        }
        v->itable[g] = it;
    }
    if(!err && want_rw){
        const char* why = rw_blocker(v, s);
        if(!why){
            err = journal_open(v);
            if(err){
                kprintf("ext4: jurnal nesuportat (%d)\n", err);
                why = "jurnal nesuportat";
                err = 0;
                if(needs_recovery){
                    err = -EINVAL;
                }
            }
        }
        if(why){
            if(!err){
                kprintf("ext4: montat read-only (%s)\n", why);
            }
            if(needs_recovery && !opts.noload){
                err = -EINVAL;
                kprintf("ext4: jurnalul cere recuperare (monteaza cu noload pentru a ignora jurnalul)\n");
            }
            want_rw = false;
            jbd2_free(v->jnl);
            v->jnl = nullptr;
        }
    }
    if(!err && want_rw && (needs_recovery || jbd2_has_log(v->jnl))){
        err = jbd2_recover(v->jnl);
        if(err){
            kprintf("ext4: reluarea jurnalului a esuat (%d)\n", err);
        } else {
            Jbd2Stats st;
            jbd2_stats(v->jnl, st);
            kprintf("ext4: jurnal reluat: %lu tranzactii, %lu blocuri (%lu sarite din cauza revoke)\n",
                    st.replayed_txns, st.replayed_blocks, st.revoked_skipped);
            // superblock-ul poate fi fost inlocuit de replay
            Buf* nb = bread(dev, 0);
            if(!nb){
                err = -EIO;
            } else {
                memcpy(v->sbraw, nb->data + 1024, 1024);
                brelse(nb);
                v->incompat = le32(v->sbraw + 0x60);
            }
        }
    }
    Node* root = nullptr;
    if(!err){
        err = node_load(v, ROOT_INO, root);
        if(!err && !S_ISDIR(root->ino->mode)){
            err = -EINVAL;
        }
    }
    if(err){
        volume_free(v);
        sb->priv = nullptr;
        return err;
    }
    v->root = root;
    sb->root_inode = root->ino;
    if(want_rw){
        recount(v);
        v->last_commit_ms = uptime_ms();
        v->commit_ms = opts.commit_s ? (uint64_t)opts.commit_s * 1000 : COMMIT_INTERVAL_MS;
        v->rw = true;
        sb->flags &= ~MS_RDONLY;
        orphan_cleanup(v);
        put32(v->sbraw + 0x60, le32(v->sbraw + 0x60) | INCOMPAT_RECOVER);
        put16(v->sbraw + 0x3A, le16(v->sbraw + 0x3A) & ~STATE_VALID);
        v->sb_dirty = true;
        if(!v->rw || tx_commit(v) || sb_write_direct(v)){
            kprintf("ext4: marcarea volumului montat a esuat\n");
            v->rw = false;
            sb->flags |= MS_RDONLY;
        } else if(!bcache_add_hook(sync_hook, v)){
            kprintf("ext4: fara loc pentru hook-ul de sincronizare (commit doar la sync / umount)\n");
        }
    }
    return 0;
}

FileSystemType ext4_type = {"ext4", e4_mount, nullptr};

}

namespace ext4 {

int orphan_add(Node* n){
    return orphan_add_(n);
}

int orphan_del(Node* n){
    Volume* v = n->vol;
    uint32_t ino = (uint32_t)n->ino->ino;
    uint32_t head = le32(v->sbraw + 0xE8);
    if(head == ino){
        put32(v->sbraw + 0xE8, n->dtime);
        v->sb_dirty = true;
        return 0;
    }
    for(uint32_t cur = head, guard = 0; cur && guard < v->inodes; guard++){
        Node* c;
        TRY(node_load(v, cur, c, true));
        if(c->dtime == ino){
            c->dtime = n->dtime;
            return inode_store(c);
        }
        cur = c->dtime;
    }
    return 0;                                   // nu era in lista
}

int node_destroy(Node* n){
    Volume* v = n->vol;
    Inode* i = n->ino;
    bool dir = S_ISDIR(i->mode);
    TRY(tx_begin(v, 40));
    if(n->flags & INODE_EXTENTS_FL){
        TRY(shrink_blocks(n, 0));
    } else if(!n->fast_symlink){
        TRY(bmap_free_all(n));
    }
    TRY(tx_begin(v, 24));
    TRY(free_acl(n));
    TRY(orphan_del(n));
    i->size = 0;
    n->dtime = (uint32_t)vfs_now().sec;
    TRY(inode_store(n));
    return ifree(v, (uint32_t)i->ino, dir);
}

}

void ext4_init(){
    fs_register(&ext4_type);
}

int ext4_mount_root(const char* dev, bool rw){
    Path none = {};
    int err = vfs_mount(none, dev, "/", "ext4", rw ? 0 : MS_RDONLY, rw ? "rw" : nullptr);
    if(!err){
        kprintf("[OK] ext4 montat pe / (%s, %s)\n", dev, rw ? "citire-scriere" : "read-only");
    }
    return err;
}

void ext4_stats(uint64_t& htree_lookups, uint64_t& linear_lookups){
    htree_lookups = stat_htree;
    linear_lookups = stat_linear;
}

bool ext4_dir_indexed(const Inode* dir){
    return (node_of(dir)->flags & INODE_INDEX_FL) != 0;
}

bool ext4_info(const Inode* any, Ext4Info& out){
    if(!any || any->sb->type != &ext4_type){
        return false;
    }
    Volume* v = node_of(any)->vol;
    MutexGuard g(v->lock);
    memset(&out, 0, sizeof(out));
    out.rw = v->rw;
    out.free_blocks = v->free_blocks;
    out.free_inodes = v->free_inodes;
    if(v->jnl){
        Jbd2Stats st;
        jbd2_stats(v->jnl, st);
        out.commits = st.commits;
        out.journaled_blocks = st.blocks;
        out.revokes = st.revokes;
        out.replayed_txns = st.replayed_txns;
        out.replayed_blocks = st.replayed_blocks;
        out.replay_revoked = st.revoked_skipped;
        out.journal_csum = st.csum_v3;
        out.sequence = jbd2_sequence(v->jnl);
        out.pending = jbd2_pending(v->jnl);
    }
    return true;
}

int ext4_commit(const Inode* any){
    if(!any || any->sb->type != &ext4_type){
        return -EINVAL;
    }
    Volume* v = node_of(any)->vol;
    MutexGuard g(v->lock);
    if(!v->rw){
        return 0;
    }
    return tx_commit(v);
}
