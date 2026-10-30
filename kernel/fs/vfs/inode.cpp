#include "fs/vfs/inode.h"
#include "sync/spinlock.h"
#include "drivers/rtc.h"
#include "mm/kmalloc.h"
#include "uapi/syscall_nr.h"

namespace {

Spinlock fs_lock("fstypes");
FileSystemType* fs_list = nullptr;
uint64_t live_inodes = 0;

bool same_name(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

}

int fs_register(FileSystemType* type){
    SpinlockGuard g(fs_lock);
    for(FileSystemType* t = fs_list; t; t = t->next){
        if(same_name(t->name, type->name)){
            return -EBUSY;
        }
    }
    type->next = fs_list;
    fs_list = type;
    return 0;
}

FileSystemType* fs_find(const char* name){
    SpinlockGuard g(fs_lock);
    for(FileSystemType* t = fs_list; t; t = t->next){
        if(same_name(t->name, name)){
            return t;
        }
    }
    return nullptr;
}

Inode* inode_alloc(SuperBlock* sb, uint64_t ino, uint32_t mode, const InodeOps* ops){
    Inode* i = (Inode*)kzalloc(sizeof(Inode));
    if(i){
        i->sb = sb;
        i->ino = ino;
        i->mode = mode;
        i->ops = ops;
        i->nlink = 1;
        i->atime = i->mtime = i->ctime = vfs_now();
        __atomic_fetch_add(&live_inodes, 1, __ATOMIC_RELAXED);
    }
    return i;
}

void inode_free(Inode* ino){
    kfree(ino);
    __atomic_fetch_sub(&live_inodes, 1, __ATOMIC_RELAXED);
}

void inode_get(Inode* ino){
    ino->refs++;
}

void inode_put(Inode* ino){
    if(--ino->refs == 0 && ino->sb->ops && ino->sb->ops->release_inode){
        ino->sb->ops->release_inode(ino);
    }
}

uint64_t inode_count(){
    return __atomic_load_n(&live_inodes, __ATOMIC_RELAXED);
}

Timespec vfs_now(){
    return Timespec{(int64_t)wall_time(), 0};
}
