#include "fs/vfs/dentry.h"
#include "mm/kmalloc.h"
#include "lib/string.h"

namespace {

const uint32_t HASH_SIZE = 256;

Dentry* hash_table[HASH_SIZE];
uint64_t live_dentries = 0;

uint32_t hash_of(const Dentry* parent, const char* name, uint32_t len){
    uint32_t h = 2166136261u ^ (uint32_t)((uintptr_t)parent >> 4);
    for(uint32_t i = 0; i < len; i++){
        h = (h ^ (uint8_t)name[i]) * 16777619u;
    }
    return h;
}

void sb_link(Dentry* d){
    SuperBlock* sb = d->sb;
    d->sb_prev = nullptr;
    d->sb_next = sb->dentries;
    if(sb->dentries){
        sb->dentries->sb_prev = d;
    }
    sb->dentries = d;
    live_dentries++;
}

void sb_unlink(Dentry* d){
    if(d->sb_prev){
        d->sb_prev->sb_next = d->sb_next;
    } else {
        d->sb->dentries = d->sb_next;
    }
    if(d->sb_next){
        d->sb_next->sb_prev = d->sb_prev;
    }
    live_dentries--;
}

Dentry* alloc(SuperBlock* sb, Dentry* parent, const char* name, uint32_t len){
    Dentry* d = (Dentry*)kzalloc(sizeof(Dentry) + len + 1);
    if(!d){
        return nullptr;
    }
    d->sb = sb;
    d->refs = 1;
    d->name_len = len;
    memcpy(d->name, name, len);
    d->name[len] = 0;
    sb_link(d);
    if(parent){
        d->parent = parent;
        parent->refs++;
        parent->nchild++;
    }
    return d;
}

void unhash(Dentry* d){
    if(!(d->flags & D_HASHED)){
        return;
    }
    Dentry** pp = &hash_table[hash_of(d->parent, d->name, d->name_len) % HASH_SIZE];
    while(*pp != d){
        pp = &(*pp)->hash_next;
    }
    *pp = d->hash_next;
    d->hash_next = nullptr;
    d->flags &= ~D_HASHED;
}

void hash_in(Dentry* d){
    Dentry** head = &hash_table[hash_of(d->parent, d->name, d->name_len) % HASH_SIZE];
    d->hash_next = *head;
    *head = d;
    d->flags |= D_HASHED;
}

// refs == 0: scoate din lista sb, elibereaza inode-ul si lasa parintelui o referinta mai putin
void release(Dentry* d){
    unhash(d);
    sb_unlink(d);
    if(d->inode){
        inode_put(d->inode);
    }
    Dentry* parent = d->parent;
    kfree(d);
    if(parent){
        parent->nchild--;
        dentry_put(parent);
    }
}

}

Dentry* d_alloc_root(SuperBlock* sb, Inode* root){
    Dentry* d = alloc(sb, nullptr, "", 0);
    if(d){
        d_instantiate(d, root);
    }
    return d;
}

int d_lookup(Dentry* dir, const char* name, uint32_t len, Dentry*& out){
    out = nullptr;
    for(Dentry* d = hash_table[hash_of(dir, name, len) % HASH_SIZE]; d; d = d->hash_next){
        if(d->parent == dir && d->name_len == len && memcmp(d->name, name, len) == 0){
            d->refs++;
            out = d;
            return 0;
        }
    }
    Inode* di = dir->inode;
    if(!di->ops || !di->ops->lookup){
        return -ENOENT;
    }
    Inode* found = nullptr;
    int err = di->ops->lookup(di, name, len, found);
    if(err && err != -ENOENT){
        return err;
    }
    Dentry* d = alloc(dir->sb, dir, name, len);
    if(!d){
        return -ENOMEM;
    }
    hash_in(d);
    if(!err){
        d_instantiate(d, found);
    }
    out = d;
    return 0;
}

void d_instantiate(Dentry* d, Inode* ino){
    inode_get(ino);
    d->inode = ino;
}

void d_drop(Dentry* d){
    unhash(d);
    d->flags |= D_DEAD;
}

void dentry_get(Dentry* d){
    d->refs++;
}

void dentry_put(Dentry* d){
    if(--d->refs == 0 && (!(d->flags & D_HASHED) || !d->inode || (d->sb->flags & SB_NOCACHE))){
        release(d);
    }
}

void d_kill_sb(SuperBlock* sb){
    // Frunzele intai (un dentry cu copii nu se poate elibera); dupa fiecare eliberare lista se reia, pentru ca
    // release poate elibera si parintele
    for(bool again = true; again;){
        again = false;
        for(Dentry* d = sb->dentries; d; d = d->sb_next){
            if(d->parent && d->nchild == 0 && d->refs == 0){
                release(d);
                again = true;
                break;
            }
        }
    }
}

uint64_t dentry_count(){
    return live_dentries;
}
