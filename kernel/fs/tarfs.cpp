#include "fs/tarfs.h"
#include "fs/vfs/vfs.h"
#include "boot/multiboot2.h"
#include "arch/x86_64/paging.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

const uint64_t BLOCK = 512;

// Campurile antetului ustar (512 bytes)
const uint64_t H_NAME = 0, H_MODE = 100, H_UID = 108, H_GID = 116, H_SIZE = 124, H_MTIME = 136, H_CHKSUM = 148,
               H_TYPE = 156, H_LINK = 157, H_MAGIC = 257, H_PREFIX = 345;

struct TarNode {
    Inode* ino;
    TarNode* first;                 // copii, in ordinea din arhiva
    TarNode* last;
    TarNode* next;                  // frate
    TarNode* all_next;
    const uint8_t* data;            // continutul fisierului sau tinta symlink-ului, direct din arhiva
    uint64_t size;
    uint8_t name_len;
    char name[];                    // fara terminator
};

struct TarFs {
    uint64_t next_ino;
    TarNode* all;
};

extern const InodeOps tar_ops;

TarNode* node_of(Inode* i){
    return (TarNode*)i->priv;
}

uint64_t field_len(const uint8_t* p, uint64_t max){
    uint64_t n = 0;
    while(n < max && p[n]){
        n++;
    }
    return n;
}

// Numar octal (sau base-256 GNU, bit 7 din primul byte) dintr-un camp de n bytes
bool parse_num(const uint8_t* p, uint64_t n, uint64_t& out){
    out = 0;
    if(p[0] & 0x80){
        for(uint64_t i = 1; i < n; i++){
            if(out >> 56){
                return false;
            }
            out = (out << 8) | p[i];
        }
        return true;
    }
    uint64_t i = 0;
    while(i < n && p[i] == ' '){
        i++;
    }
    for(; i < n && p[i] >= '0' && p[i] <= '7'; i++){
        out = (out << 3) | (uint64_t)(p[i] - '0');
    }
    return true;
}

bool zero_block(const uint8_t* h){
    for(uint64_t i = 0; i < BLOCK; i++){
        if(h[i]){
            return false;
        }
    }
    return true;
}

bool checksum_ok(const uint8_t* h){
    uint64_t sum = 0;
    for(uint64_t i = 0; i < BLOCK; i++){
        sum += (i >= H_CHKSUM && i < H_CHKSUM + 8) ? ' ' : h[i];
    }
    uint64_t want;
    return parse_num(h + H_CHKSUM, 8, want) && want == sum;
}

TarNode* child_find(TarNode* dir, const char* name, uint64_t len){
    for(TarNode* n = dir->first; n; n = n->next){
        if(n->name_len == len && memcmp(n->name, name, len) == 0){
            return n;
        }
    }
    return nullptr;
}

TarNode* node_new(SuperBlock* sb, TarNode* parent, const char* name, uint64_t len, uint32_t mode){
    TarFs* fs = (TarFs*)sb->priv;
    TarNode* n = (TarNode*)kzalloc(sizeof(TarNode) + len);
    Inode* i = n ? inode_alloc(sb, fs->next_ino, mode, &tar_ops) : nullptr;
    if(!i){
        kfree(n);
        return nullptr;
    }
    fs->next_ino++;
    i->priv = n;
    n->ino = i;
    n->name_len = (uint8_t)len;
    memcpy(n->name, name, len);
    n->all_next = fs->all;
    fs->all = n;
    if(S_ISDIR(mode)){
        i->nlink = 2;
    }
    if(parent){
        if(parent->last){
            parent->last->next = n;
        } else {
            parent->first = n;
        }
        parent->last = n;
        if(S_ISDIR(mode)){
            parent->ino->nlink++;
        }
    }
    return n;
}

struct Entry {
    char type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime;
    const uint8_t* data;
    uint64_t size;
};

void apply_meta(TarNode* n, const Entry& e){
    Inode* i = n->ino;
    i->mode = (i->mode & S_IFMT) | e.mode;
    i->uid = e.uid;
    i->gid = e.gid;
    i->atime = i->mtime = i->ctime = Timespec{(int64_t)e.mtime, 0};
}

// Nodul de la calea path (relativa la radacina, fara "." / ".."); nullptr daca lipseste
TarNode* find_path(TarNode* root, const char* path, uint64_t len){
    TarNode* cur = root;
    uint64_t i = 0;
    while(cur && i < len){
        while(i < len && path[i] == '/'){
            i++;
        }
        uint64_t s = i;
        while(i < len && path[i] != '/'){
            i++;
        }
        if(i > s){
            cur = S_ISDIR(cur->ino->mode) ? child_find(cur, path + s, i - s) : nullptr;
        }
    }
    return cur;
}

// Adauga o intrare la calea path (director-ele lipsa din cale se creeaza implicit). 0 sau -errno;
// -EEXIST = intrare duplicata (ignorata)
int add_entry(SuperBlock* sb, TarNode* root, const char* path, uint64_t len, const Entry& e, TarNode* link_to){
    TarNode* cur = root;
    uint64_t i = 0;
    for(;;){
        while(i < len && path[i] == '/'){
            i++;
        }
        if(i >= len){
            break;
        }
        uint64_t s = i;
        while(i < len && path[i] != '/'){
            i++;
        }
        uint64_t n = i - s;
        const char* c = path + s;
        if(n == 1 && c[0] == '.'){
            continue;
        }
        if(n > NAME_MAX || (n == 2 && c[0] == '.' && c[1] == '.')){
            return -EINVAL;
        }
        bool is_last = true;
        for(uint64_t j = i; j < len; j++){
            if(path[j] != '/'){
                is_last = false;
                break;
            }
        }
        TarNode* found = child_find(cur, c, n);
        if(!is_last){
            if(!found){
                found = node_new(sb, cur, c, n, S_IFDIR | 0755);
                if(!found){
                    return -ENOMEM;
                }
            } else if(!S_ISDIR(found->ino->mode)){
                return -ENOTDIR;
            }
            cur = found;
            continue;
        }
        if(found){
            if(e.type == '5' && S_ISDIR(found->ino->mode)){
                apply_meta(found, e);       // director aparut intai implicit
                return 0;
            }
            return -EEXIST;
        }
        uint32_t kind = e.type == '5' ? S_IFDIR : e.type == '2' ? S_IFLNK : S_IFREG;
        TarNode* nn = node_new(sb, cur, c, n, kind | e.mode);
        if(!nn){
            return -ENOMEM;
        }
        apply_meta(nn, e);
        nn->data = link_to ? link_to->data : e.data;
        nn->size = link_to ? link_to->size : e.size;
        nn->ino->size = nn->size;
        return 0;
    }
    // Calea nu are componente: "." = radacina
    if(e.type == '5'){
        apply_meta(root, e);
    }
    return 0;
}

int tar_parse(SuperBlock* sb, TarNode* root, const uint8_t* a, uint64_t size){
    uint64_t off = 0;
    uint64_t count = 0;
    while(off + BLOCK <= size){
        const uint8_t* h = a + off;
        if(zero_block(h)){
            break;
        }
        if(memcmp(h + H_MAGIC, "ustar", 5) != 0 || !checksum_ok(h)){
            kprintf("tarfs: antet invalid la offset %lu\n", off);
            return -EINVAL;
        }
        Entry e = {};
        uint64_t mode, uid, gid, fsize, mtime;
        if(!parse_num(h + H_MODE, 8, mode) || !parse_num(h + H_UID, 8, uid) || !parse_num(h + H_GID, 8, gid) ||
           !parse_num(h + H_SIZE, 12, fsize) || !parse_num(h + H_MTIME, 12, mtime)){
            return -EINVAL;
        }
        e.type = (char)h[H_TYPE] ? (char)h[H_TYPE] : '0';
        e.mode = (uint32_t)mode & 07777;
        e.uid = (uint32_t)uid;
        e.gid = (uint32_t)gid;
        e.mtime = mtime;

        // Doar fisierele (si tipurile necunoscute, ca extensii pax/GNU) au date dupa antet
        bool has_data = e.type != '1' && e.type != '2' && e.type != '5';
        uint64_t body = off + BLOCK;
        uint64_t padded = has_data ? (fsize + BLOCK - 1) / BLOCK * BLOCK : 0;
        if(has_data && (body > size || fsize > size - body)){
            kprintf("tarfs: arhiva trunchiata la offset %lu\n", off);
            return -EINVAL;
        }
        char path[260];
        uint64_t plen = field_len(h + H_PREFIX, 155);
        memcpy(path, h + H_PREFIX, plen);
        if(plen){
            path[plen++] = '/';
        }
        uint64_t nlen = field_len(h + H_NAME, 100);
        memcpy(path + plen, h + H_NAME, nlen);
        plen += nlen;

        TarNode* link_to = nullptr;
        bool supported = true;
        switch(e.type){
        case '0': case '7':
            e.data = a + body;
            e.size = fsize;
            break;
        case '5':
            break;
        case '2':
            e.data = h + H_LINK;
            e.size = field_len(h + H_LINK, 100);
            break;
        case '1': {
            link_to = find_path(root, (const char*)h + H_LINK, field_len(h + H_LINK, 100));
            supported = link_to && S_ISREG(link_to->ino->mode);
            break;
        }
        default:
            supported = false;      // dispozitive, fifo, extensii pax/GNU
            break;
        }
        if(supported){
            int err = add_entry(sb, root, path, plen, e, link_to);
            if(err == -ENOMEM){
                return err;
            }
            if(err == 0){
                count++;
            }
        }
        off = body + padded;
    }
    kprintf("tarfs: %lu intrari\n", count);
    return 0;
}

// ---- operatiile pe inode-uri (read-only) ----

int tar_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    TarNode* n = child_find(node_of(dir), name, len);
    if(!n){
        return -ENOENT;
    }
    out = n->ino;
    return 0;
}

int tar_create(Inode*, const char*, uint32_t, uint32_t, Inode*&){
    return -EROFS;
}

int tar_symlink(Inode*, const char*, uint32_t, const char*, Inode*&){
    return -EROFS;
}

int tar_unlink(Inode*, const char*, uint32_t, Inode*){
    return -EROFS;
}

int64_t tar_read(Inode* ino, uint64_t pos, void* buf, uint64_t len){
    TarNode* n = node_of(ino);
    if(pos >= n->size){
        return 0;
    }
    if(len > n->size - pos){
        len = n->size - pos;
    }
    memcpy(buf, n->data + pos, len);
    return (int64_t)len;
}

int64_t tar_write(Inode*, uint64_t, const void*, uint64_t){
    return -EROFS;
}

int tar_truncate(Inode*, uint64_t){
    return -EROFS;
}

int tar_readdir(Inode* dir, uint64_t index, DirEntry& out){
    TarNode* n = node_of(dir)->first;
    for(; n && index; index--){
        n = n->next;
    }
    if(!n){
        return 0;
    }
    out.ino = n->ino->ino;
    out.type = S_ISDIR(n->ino->mode) ? DT_DIR : S_ISLNK(n->ino->mode) ? DT_LNK : DT_REG;
    out.name_len = n->name_len;
    memcpy(out.name, n->name, n->name_len);
    out.name[n->name_len] = 0;
    return 1;
}

int64_t tar_readlink(Inode* ino, char* buf, uint64_t len){
    TarNode* n = node_of(ino);
    if(len > n->size){
        len = n->size;
    }
    memcpy(buf, n->data, len);
    return (int64_t)len;
}

const InodeOps tar_ops = {tar_lookup, tar_create, tar_create, tar_symlink, tar_unlink, tar_unlink,
                          tar_read, tar_write, tar_truncate, tar_readdir, tar_readlink};

// Inode-urile raman ale fs-ului pana la umount (nlink >= 1)
void tar_release(Inode*){
}

void tar_kill(SuperBlock* sb){
    TarFs* fs = (TarFs*)sb->priv;
    while(fs->all){
        TarNode* n = fs->all;
        fs->all = n->all_next;
        inode_free(n->ino);
        kfree(n);
    }
    kfree(fs);
}

const SuperBlockOps tar_sb_ops = {tar_release, tar_kill};

int tar_mount(FileSystemType*, SuperBlock* sb, const char* dev_name, const char*){
    const BootModule* m = dev_name ? boot_find_module(dev_name) : nullptr;
    if(!m){
        return -ENOENT;
    }
    TarFs* fs = (TarFs*)kzalloc(sizeof(TarFs));
    if(!fs){
        return -ENOMEM;
    }
    fs->next_ino = 1;
    sb->ops = &tar_sb_ops;
    sb->priv = fs;
    sb->flags |= MS_RDONLY;
    TarNode* root = node_new(sb, nullptr, "", 0, S_IFDIR | 0755);
    int err = root ? tar_parse(sb, root, (const uint8_t*)phys_to_virt(m->start), m->end - m->start) : -ENOMEM;
    if(err){
        tar_kill(sb);
        return err;
    }
    sb->root_inode = root->ino;
    return 0;
}

FileSystemType tarfs_type = {"tarfs", tar_mount, nullptr};

}

void tarfs_init(){
    fs_register(&tarfs_type);
}

int tarfs_mount_root(){
    Path none = {};
    return vfs_mount(none, "initrd", "/", "tarfs", MS_RDONLY, nullptr);
}
