#include "fs/vfs/file.h"
#include "fs/vfs/vfs.h"
#include "uapi/syscall_nr.h"
#include "lib/string.h"

namespace {

int64_t file_read(File* f, void* buf, uint64_t len, bool){
    Inode* i = f->inode;
    if((f->flags & O_ACCMODE) == O_WRONLY){
        return -EBADF;
    }
    if(S_ISDIR(i->mode)){
        return -EISDIR;
    }
    if(!i->ops || !i->ops->read){
        return -EINVAL;
    }
    int64_t n = i->ops->read(i, __atomic_load_n(&f->pos, __ATOMIC_RELAXED), buf, len);
    if(n > 0){
        __atomic_fetch_add(&f->pos, (uint64_t)n, __ATOMIC_RELAXED);
    }
    return n;
}

int64_t file_write(File* f, const void* buf, uint64_t len, bool){
    Inode* i = f->inode;
    if((f->flags & O_ACCMODE) == O_RDONLY){
        return -EBADF;
    }
    if(S_ISDIR(i->mode)){
        return -EISDIR;
    }
    if(!i->ops || !i->ops->write){
        return -EINVAL;
    }
    uint64_t pos = (f->flags & O_APPEND) ? __atomic_load_n(&i->size, __ATOMIC_RELAXED)
                                         : __atomic_load_n(&f->pos, __ATOMIC_RELAXED);
    int64_t n = i->ops->write(i, pos, buf, len);
    if(n > 0){
        __atomic_store_n(&f->pos, pos + (uint64_t)n, __ATOMIC_RELAXED);
        i->mtime = i->ctime = vfs_now();
    }
    return n;
}

void file_close(File* f){
    MutexGuard g(vfs_mutex());
    dentry_put(f->dentry);
}

int64_t file_lseek(File* f, int64_t off, int whence){
    int64_t base;
    switch(whence){
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t)__atomic_load_n(&f->pos, __ATOMIC_RELAXED); break;
        case SEEK_END: base = (int64_t)__atomic_load_n(&f->inode->size, __ATOMIC_RELAXED); break;
        default: return -EINVAL;
    }
    // Un director se poate doar rebobina (pozitia lui e indexul intrarii urmatoare)
    if(S_ISDIR(f->inode->mode) && (whence != SEEK_SET || off < 0)){
        return -EINVAL;
    }
    int64_t r = base + off;
    if(r < 0){
        return -EINVAL;
    }
    __atomic_store_n(&f->pos, (uint64_t)r, __ATOMIC_RELAXED);
    return r;
}

int64_t file_getdents(File* f, void* buf, uint64_t len){
    Inode* dir = f->inode;
    if(!S_ISDIR(dir->mode)){
        return -ENOTDIR;
    }
    MutexGuard g(vfs_mutex());
    uint64_t pos = f->pos;
    uint64_t used = 0;
    for(;;){
        DirEntry e;
        if(pos < 2){
            Dentry* par = f->dentry->parent;
            e.ino = pos == 1 && par && par->inode ? par->inode->ino : dir->ino;
            e.type = DT_DIR;
            e.name_len = (uint8_t)(pos + 1);
            e.name[0] = '.';
            e.name[1] = pos ? '.' : 0;
            e.name[2] = 0;
        } else {
            int r = dir->ops && dir->ops->readdir ? dir->ops->readdir(dir, pos - 2, e) : 0;
            if(r < 0 && !used){
                return r;
            }
            if(r <= 0){
                break;
            }
        }
        uint64_t reclen = (__builtin_offsetof(Dirent64, d_name) + e.name_len + 1 + 7) & ~7UL;
        if(used + reclen > len){
            if(!used){
                return -EINVAL;
            }
            break;
        }
        Dirent64* d = (Dirent64*)((char*)buf + used);
        d->d_ino = e.ino;
        d->d_off = (int64_t)pos + 1;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = e.type;
        memcpy(d->d_name, e.name, e.name_len);
        d->d_name[e.name_len] = 0;
        used += reclen;
        pos++;
    }
    f->pos = pos;
    return (int64_t)used;
}

}

const FileOps vfs_file_ops = {file_read, file_write, file_close, file_lseek, file_getdents};

File* vfs_file_new(const Path& p, uint32_t flags){
    File* f = file_alloc(&vfs_file_ops, nullptr);
    if(f){
        f->inode = p.dentry->inode;
        f->dentry = p.dentry;
        f->flags = flags;
    }
    return f;
}

void vfs_fill_stat(const Inode* ino, Stat& st){
    memset(&st, 0, sizeof(st));
    st.st_dev = ino->sb->dev;
    st.st_ino = ino->ino;
    st.st_nlink = ino->nlink;
    st.st_mode = ino->mode;
    st.st_uid = ino->uid;
    st.st_gid = ino->gid;
    st.st_size = (int64_t)ino->size;
    st.st_blksize = 4096;
    st.st_blocks = (int64_t)((ino->size + 511) / 512);
    st.st_atim = ino->atime;
    st.st_mtim = ino->mtime;
    st.st_ctim = ino->ctime;
}
