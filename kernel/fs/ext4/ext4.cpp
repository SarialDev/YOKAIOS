#include "fs/ext4/ext4.h"
#include "fs/ext4/ext4_priv.h"
#include "fs/vfs/vfs.h"
#include "block/blockdev.h"
#include "hal/hal.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/new.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

using namespace ext4;

namespace {

extern const InodeOps ext4_ops;
extern const SuperBlockOps ext4_sb_ops;

Node* node_of(const Inode* i){
    return (Node*)i->priv;
}

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

// Inode-ul ino (incarcat o singura data; ramane in memorie). Cu lock-ul volumului luat
int node_load(Volume* v, uint32_t ino, Node*& out){
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
    uint32_t idx = (ino - 1) % v->ipg;
    if(g >= v->groups){
        return -EIO;
    }
    uint8_t raw[160];
    uint32_t rl = v->inode_size < sizeof(raw) ? v->inode_size : sizeof(raw);
    int err = read_bytes(v, (v->itable[g] << v->bs_shift) + (uint64_t)idx * v->inode_size, raw, rl);
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
    if(links == 0 || (flags & INODE_INLINE_FL)){
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
    Inode* i = inode_alloc(v->sb, ino, mode, &ext4_ops);
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
    memcpy(n->blk, raw + 40, 60);
    uint32_t blocks512 = le32(raw + 28);
    uint32_t ea = le32(raw + 104) ? v->bs >> 9 : 0;
    n->fast_symlink = type == S_IFLNK && blocks512 == ea;
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

// ---- operatii pe inode-uri ----

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
    return node_read(node_of(ino), pos, buf, len);
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
    return node_read(n, 0, buf, take);
}

int e4_create(Inode*, const char*, uint32_t, uint32_t, Inode*&){
    return -EROFS;
}

int e4_symlink(Inode*, const char*, uint32_t, const char*, Inode*&){
    return -EROFS;
}

int e4_unlink(Inode*, const char*, uint32_t, Inode*){
    return -EROFS;
}

int64_t e4_write(Inode*, uint64_t, const void*, uint64_t){
    return -EROFS;
}

int e4_truncate(Inode*, uint64_t){
    return -EROFS;
}

int e4_rename(Inode*, const char*, uint32_t, Inode*, const char*, uint32_t, Inode*, Inode*){
    return -EROFS;
}

const InodeOps ext4_ops = {e4_lookup, e4_create, e4_create, e4_symlink, e4_unlink, e4_unlink, e4_read, e4_write,
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
    node_unlink(v, n);
    inode_free(i);
    kfree(n);
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
    kfree(v->itable);
    kfree(v);
}

void e4_kill(SuperBlock* sb){
    Volume* v = (Volume*)sb->priv;
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

// Grupul g are copie a superblock-ului / descriptorilor (sparse_super: 0, 1 si puterile lui 3, 5, 7)
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

int parse_options(const char* data, bool& noload){
    while(data && *data){
        if(data[0] == 'r' && data[1] == 'o' && (data[2] == ',' || !data[2])){
        } else if(memcmp(data, "noload", 6) == 0 && (data[6] == ',' || !data[6])){
            noload = true;
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

int e4_mount(FileSystemType*, SuperBlock* sb, const char* dev_name, const char* data){
    BlockDevice* dev = find_block(dev_name);
    if(!dev){
        return -ENODEV;
    }
    bool noload = false;
    int err = parse_options(data, noload);
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
    if((incompat & INCOMPAT_RECOVER) && !noload){
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
    v->itable = (uint64_t*)kzalloc((size_t)groups * sizeof(uint64_t));
    if(!v->itable){
        kfree(v);
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
        err = blk >= blocks ? -EIO : read_bytes(v, (blk << v->bs_shift) + (uint64_t)(g % dpb) * desc, gd, take);
        if(err){
            break;
        }
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
    return 0;
}

FileSystemType ext4_type = {"ext4", e4_mount, nullptr};

}

void ext4_init(){
    fs_register(&ext4_type);
}

int ext4_mount_root(const char* dev){
    Path none = {};
    int err = vfs_mount(none, dev, "/", "ext4", MS_RDONLY, nullptr);
    if(!err){
        kprintf("[OK] ext4 montat pe / (%s, read-only)\n", dev);
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
