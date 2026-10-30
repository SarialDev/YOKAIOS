#include "fs/pseudofs.h"
#include "fs/vfs/vfs.h"
#include "arch/x86_64/paging.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

// Datele unui fisier: tabela de nivel 1 (o pagina, 512 pointeri fizici) -> tabele frunza (512 pointeri) -> pagini
// de date. Intrarea 0 = gaura (se citeste ca zerouri, nu ocupa memorie). Paginile se aloca la prima scriere
const uint64_t PTRS = PAGE_SIZE / 8;
const uint64_t MAX_FILE = PTRS * PTRS * PAGE_SIZE;      // 1 GiB

struct TmpNode;

struct TmpEntry {
    TmpEntry* next;
    TmpNode* node;
    uint8_t len;
    char name[];                    // fara terminator
};

struct TmpNode {
    Inode* ino = nullptr;
    TmpNode* prev = nullptr;        // lista tuturor nodurilor, sub lock-ul VFS
    TmpNode* next = nullptr;
    Mutex lock;                     // datele fisierului (read/write nu iau lock-ul VFS)
    uint64_t top_phys = 0;
    uint64_t* top = nullptr;
    TmpEntry* ents = nullptr;       // director
    TmpEntry* last = nullptr;
    char* target = nullptr;         // symlink
};

struct TmpFs {
    TmpNode* all;
    uint64_t next_ino;
    uint64_t max_pages;             // date + tabele
    uint64_t used_pages;
};

extern const InodeOps tmp_ops;
extern const SuperBlockOps tmp_sb_ops;

TmpNode* node_of(Inode* i){
    return (TmpNode*)i->priv;
}

TmpFs* fs_of(Inode* i){
    return (TmpFs*)i->sb->priv;
}

// Pagina zerorizata; 0 daca s-a atins limita fs-ului sau nu mai e memorie
uint64_t page_alloc(TmpFs* fs){
    if(__atomic_add_fetch(&fs->used_pages, 1, __ATOMIC_RELAXED) > fs->max_pages){
        __atomic_sub_fetch(&fs->used_pages, 1, __ATOMIC_RELAXED);
        return 0;
    }
    uint64_t p = pmm_alloc_page();
    if(!p){
        __atomic_sub_fetch(&fs->used_pages, 1, __ATOMIC_RELAXED);
        return 0;
    }
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

void page_free(TmpFs* fs, uint64_t p){
    pmm_free_page(p);
    __atomic_sub_fetch(&fs->used_pages, 1, __ATOMIC_RELAXED);
}

// Pagina de date numarul pi sau nullptr (gaura, sau -ENOSPC/-ENOMEM in err daca create nu a reusit). Lock-ul nodului luat
uint8_t* data_page(TmpFs* fs, TmpNode* n, uint64_t pi, bool create, int& err){
    err = 0;
    if(!n->top){
        if(!create){
            return nullptr;
        }
        uint64_t p = page_alloc(fs);
        if(!p){
            err = -ENOSPC;
            return nullptr;
        }
        n->top_phys = p;
        n->top = (uint64_t*)phys_to_virt(p);
    }
    uint64_t& l1 = n->top[pi / PTRS];
    if(!l1){
        if(!create){
            return nullptr;
        }
        l1 = page_alloc(fs);
        if(!l1){
            err = -ENOSPC;
            return nullptr;
        }
    }
    uint64_t& l2 = ((uint64_t*)phys_to_virt(l1))[pi % PTRS];
    if(!l2){
        if(!create){
            return nullptr;
        }
        l2 = page_alloc(fs);
        if(!l2){
            err = -ENOSPC;
            return nullptr;
        }
    }
    return (uint8_t*)phys_to_virt(l2);
}

// Elibereaza paginile de date de la numarul from in sus (si tabelele ramase goale). Lock-ul nodului luat
void pages_free_from(TmpFs* fs, TmpNode* n, uint64_t from){
    if(!n->top){
        return;
    }
    for(uint64_t a = from / PTRS; a < PTRS; a++){
        if(!n->top[a]){
            continue;
        }
        uint64_t* leaf = (uint64_t*)phys_to_virt(n->top[a]);
        uint64_t b0 = a == from / PTRS ? from % PTRS : 0;
        for(uint64_t b = b0; b < PTRS; b++){
            if(leaf[b]){
                page_free(fs, leaf[b]);
                leaf[b] = 0;
            }
        }
        if(b0 == 0){
            page_free(fs, n->top[a]);
            n->top[a] = 0;
        }
    }
    if(from == 0){
        page_free(fs, n->top_phys);
        n->top = nullptr;
        n->top_phys = 0;
    }
}

void entries_free(TmpNode* n){
    while(n->ents){
        TmpEntry* e = n->ents;
        n->ents = e->next;
        kfree(e);
    }
    n->last = nullptr;
}

void node_destroy(TmpNode* n){
    TmpFs* fs = fs_of(n->ino);
    pages_free_from(fs, n, 0);
    entries_free(n);
    kfree(n->target);
    if(n->prev){
        n->prev->next = n->next;
    } else {
        fs->all = n->next;
    }
    if(n->next){
        n->next->prev = n->prev;
    }
    inode_free(n->ino);
    kfree(n);
}

TmpNode* node_new(SuperBlock* sb, uint32_t mode){
    TmpFs* fs = (TmpFs*)sb->priv;
    void* mem = kzalloc(sizeof(TmpNode));
    if(!mem){
        return nullptr;
    }
    TmpNode* n = new (mem) TmpNode();
    Inode* i = inode_alloc(sb, fs->next_ino, mode, &tmp_ops);
    if(!i){
        kfree(n);
        return nullptr;
    }
    fs->next_ino++;
    i->priv = n;
    n->ino = i;
    if(S_ISDIR(mode)){
        i->nlink = 2;
    }
    n->next = fs->all;
    if(fs->all){
        fs->all->prev = n;
    }
    fs->all = n;
    return n;
}

TmpEntry* entry_find(TmpNode* dir, const char* name, uint32_t len, TmpEntry** before = nullptr){
    TmpEntry* prev = nullptr;
    for(TmpEntry* e = dir->ents; e; prev = e, e = e->next){
        if(e->len == len && memcmp(e->name, name, len) == 0){
            if(before){
                *before = prev;
            }
            return e;
        }
    }
    return nullptr;
}

// ---- operatii pe inode-uri ----

int tmp_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    TmpEntry* e = entry_find(node_of(dir), name, len);
    if(!e){
        return -ENOENT;
    }
    out = e->node->ino;
    return 0;
}

// Nod nou legat in dir; la eroare nodul se elibereaza
int tmp_make(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out, const char* target = nullptr){
    TmpNode* d = node_of(dir);
    if(len == 0 || len > NAME_MAX){
        return len ? -ENAMETOOLONG : -EINVAL;
    }
    if(entry_find(d, name, len)){
        return -EEXIST;
    }
    TmpNode* n = node_new(dir->sb, mode);
    TmpEntry* e = (TmpEntry*)kmalloc(sizeof(TmpEntry) + len);
    int err = n && e ? 0 : -ENOMEM;
    if(!err && target){
        uint64_t tl = 0;
        while(target[tl]){
            tl++;
        }
        n->target = (char*)kmalloc(tl);
        if(n->target){
            memcpy(n->target, target, tl);
            n->ino->size = tl;
        } else {
            err = -ENOMEM;
        }
    }
    if(err){
        kfree(e);
        if(n){
            n->ino->nlink = 0;
            node_destroy(n);
        }
        return err;
    }
    e->next = nullptr;
    e->node = n;
    e->len = (uint8_t)len;
    memcpy(e->name, name, len);
    if(d->last){
        d->last->next = e;
    } else {
        d->ents = e;
    }
    d->last = e;
    if(S_ISDIR(mode)){
        dir->nlink++;
    }
    dir->mtime = dir->ctime = vfs_now();
    out = n->ino;
    return 0;
}

int tmp_create(Inode* dir, const char* name, uint32_t len, uint32_t mode, Inode*& out){
    return tmp_make(dir, name, len, mode, out);
}

int tmp_symlink(Inode* dir, const char* name, uint32_t len, const char* target, Inode*& out){
    return tmp_make(dir, name, len, S_IFLNK | 0777, out, target);
}

int tmp_remove(Inode* dir, const char* name, uint32_t len, Inode* victim, bool is_dir){
    TmpNode* d = node_of(dir);
    TmpEntry* before;
    TmpEntry* e = entry_find(d, name, len, &before);
    if(!e || e->node->ino != victim){
        return -ENOENT;
    }
    if(is_dir){
        if(e->node->ents){
            return -ENOTEMPTY;
        }
        dir->nlink--;
        victim->nlink = 0;
    } else {
        victim->nlink--;
    }
    if(before){
        before->next = e->next;
    } else {
        d->ents = e->next;
    }
    if(d->last == e){
        d->last = before;
    }
    kfree(e);
    dir->mtime = dir->ctime = vfs_now();
    return 0;
}

int tmp_unlink(Inode* dir, const char* name, uint32_t len, Inode* victim){
    return tmp_remove(dir, name, len, victim, false);
}

int tmp_rmdir(Inode* dir, const char* name, uint32_t len, Inode* victim){
    return tmp_remove(dir, name, len, victim, true);
}

int64_t tmp_read(Inode* i, uint64_t pos, void* buf, uint64_t len){
    TmpNode* n = node_of(i);
    TmpFs* fs = fs_of(i);
    MutexGuard g(n->lock);
    uint64_t size = i->size;
    if(pos >= size){
        return 0;
    }
    if(len > size - pos){
        len = size - pos;
    }
    uint64_t done = 0;
    while(done < len){
        uint64_t p = pos + done;
        uint64_t off = p % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - off < len - done ? PAGE_SIZE - off : len - done;
        int err;
        uint8_t* pg = data_page(fs, n, p / PAGE_SIZE, false, err);
        if(pg){
            memcpy((uint8_t*)buf + done, pg + off, chunk);
        } else {
            memset((uint8_t*)buf + done, 0, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

int64_t tmp_write(Inode* i, uint64_t pos, const void* buf, uint64_t len){
    if(len == 0){
        return 0;
    }
    if(pos >= MAX_FILE){
        return -EFBIG;
    }
    if(len > MAX_FILE - pos){
        len = MAX_FILE - pos;
    }
    TmpNode* n = node_of(i);
    TmpFs* fs = fs_of(i);
    MutexGuard g(n->lock);
    uint64_t done = 0;
    while(done < len){
        uint64_t p = pos + done;
        uint64_t off = p % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - off < len - done ? PAGE_SIZE - off : len - done;
        int err;
        uint8_t* pg = data_page(fs, n, p / PAGE_SIZE, true, err);
        if(!pg){
            if(!done){
                return err;
            }
            break;
        }
        memcpy(pg + off, (const uint8_t*)buf + done, chunk);
        done += chunk;
    }
    if(pos + done > i->size){
        __atomic_store_n(&i->size, pos + done, __ATOMIC_RELAXED);
    }
    return (int64_t)done;
}

// Octetii de dupa size, din ultima pagina, raman mereu zero: extinderea (truncate sau scriere dupa sfarsit) ii expune
int tmp_truncate(Inode* i, uint64_t size){
    if(size > MAX_FILE){
        return -EFBIG;
    }
    TmpNode* n = node_of(i);
    TmpFs* fs = fs_of(i);
    MutexGuard g(n->lock);
    if(size < i->size){
        uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        pages_free_from(fs, n, keep);
        if(size % PAGE_SIZE){
            int err;
            uint8_t* pg = data_page(fs, n, size / PAGE_SIZE, false, err);
            if(pg){
                memset(pg + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
            }
        }
    }
    __atomic_store_n(&i->size, size, __ATOMIC_RELAXED);
    return 0;
}

int tmp_readdir(Inode* dir, uint64_t index, DirEntry& out){
    TmpEntry* e = node_of(dir)->ents;
    for(; e && index; index--){
        e = e->next;
    }
    if(!e){
        return 0;
    }
    uint32_t m = e->node->ino->mode;
    out.ino = e->node->ino->ino;
    out.type = S_ISDIR(m) ? DT_DIR : S_ISLNK(m) ? DT_LNK : DT_REG;
    out.name_len = e->len;
    memcpy(out.name, e->name, e->len);
    out.name[e->len] = 0;
    return 1;
}

int64_t tmp_readlink(Inode* i, char* buf, uint64_t len){
    TmpNode* n = node_of(i);
    if(len > i->size){
        len = i->size;
    }
    memcpy(buf, n->target, len);
    return (int64_t)len;
}

const InodeOps tmp_ops = {tmp_lookup, tmp_create, tmp_create, tmp_symlink, tmp_unlink, tmp_rmdir,
                          tmp_read, tmp_write, tmp_truncate, tmp_readdir, tmp_readlink};

// Un inode scos din arbore (nlink 0) dispare la ultima referinta; restul raman ale fs-ului pana la kill
void tmp_release(Inode* i){
    if(i->nlink == 0){
        node_destroy(node_of(i));
    }
}

void tmp_kill(SuperBlock* sb){
    TmpFs* fs = (TmpFs*)sb->priv;
    while(fs->all){
        fs->all->ino->nlink = 0;
        node_destroy(fs->all);
    }
    kfree(fs);
}

const SuperBlockOps tmp_sb_ops = {tmp_release, tmp_kill};

bool parse_num(const char*& p, uint64_t base, uint64_t& out){
    out = 0;
    const char* s = p;
    for(; *s; s++){
        uint64_t d = *s >= '0' && *s <= '9' ? (uint64_t)(*s - '0') : 99;
        if(d >= base){
            break;
        }
        out = out * base + d;
    }
    bool ok = s != p;
    p = s;
    return ok;
}

// data: "size=<KiB>,mode=<octal>" (ambele optionale)
int parse_options(const char* data, uint64_t& max_pages, uint32_t& mode){
    while(data && *data){
        uint64_t v;
        if(memcmp(data, "size=", 5) == 0){
            data += 5;
            if(!parse_num(data, 10, v)){
                return -EINVAL;
            }
            max_pages = (v * 1024 + PAGE_SIZE - 1) / PAGE_SIZE;
        } else if(memcmp(data, "mode=", 5) == 0){
            data += 5;
            if(!parse_num(data, 8, v) || v > 07777){
                return -EINVAL;
            }
            mode = (uint32_t)v;
        } else {
            return -EINVAL;
        }
        if(*data == ','){
            data++;
        } else if(*data){
            return -EINVAL;
        }
    }
    return 0;
}

int tmp_mount(FileSystemType*, SuperBlock* sb, const char*, const char* data){
    uint64_t max_pages = pmm_total_pages() / 2;
    uint32_t mode = 01777;
    int err = parse_options(data, max_pages, mode);
    if(err){
        return err;
    }
    TmpFs* fs = (TmpFs*)kzalloc(sizeof(TmpFs));
    if(!fs){
        return -ENOMEM;
    }
    fs->next_ino = 1;
    fs->max_pages = max_pages;
    sb->ops = &tmp_sb_ops;
    sb->priv = fs;
    TmpNode* root = node_new(sb, S_IFDIR | mode);
    if(!root){
        kfree(fs);
        return -ENOMEM;
    }
    sb->root_inode = root->ino;
    return 0;
}

FileSystemType tmpfs_type = {"tmpfs", tmp_mount, nullptr};

}

void tmpfs_init(){
    fs_register(&tmpfs_type);
}
