#include "fs/vfs/vfs.h"
#include "uapi/syscall_nr.h"
#include "lib/kprintf.h"

namespace {

Mutex lock("vfs");

bool dot_name(const LastName& l){
    return l.len == 1 && l.name[0] == '.';
}

bool dotdot_name(const LastName& l){
    return l.len == 2 && l.name[0] == '.' && l.name[1] == '.';
}

// Directorul parinte si dentry-ul ultimei componente (pozitiv sau negativ). Numele goale, "." si ".." intorc 0 cu
// child == nullptr: fiecare operatie decide ce eroare dau
int parent_and_child(const Path& base, const char* path, Path& dir, LastName& last, Dentry*& child){
    child = nullptr;
    int err = path_lookup(base, path, LOOKUP_PARENT, dir, &last);
    if(err){
        return err;
    }
    if(last.len == 0 || dot_name(last) || dotdot_name(last)){
        return 0;
    }
    err = d_lookup(dir.dentry, last.name, last.len, child);
    if(err){
        path_put(dir);
    }
    return err;
}

// Fisierul numit de path, cu O_CREAT. created = a fost creat acum. -EAGAIN: calea nu poate fi creata
// (. , .. , / sau symlink in ultima componenta) si se rezolva ca un open obisnuit
int open_create(const Path& base, const char* path, int flags, uint32_t mode, Path& out, bool& created){
    Path dir;
    LastName last;
    Dentry* child;
    int err = parent_and_child(base, path, dir, last, child);
    if(err){
        return err;
    }
    if(!child){
        path_put(dir);
        return flags & O_EXCL ? -EEXIST : -EAGAIN;
    }
    Inode* di = dir.dentry->inode;
    if(child->inode){
        Path p = {dir.mnt, child};
        path_put(dir);
        if(flags & O_EXCL){
            path_put(p);
            return -EEXIST;
        }
        follow_mounts(p);
        if(S_ISLNK(p.dentry->inode->mode) && !(flags & O_NOFOLLOW)){
            path_put(p);
            return -EAGAIN;
        }
        out = p;
        return 0;
    }
    Inode* ni = nullptr;
    if(last.trailing_slash){
        err = -EISDIR;
    } else if(!di->ops || !di->ops->create){
        err = -EPERM;
    } else {
        err = di->ops->create(di, last.name, last.len, S_IFREG | (mode & 07777), ni);
    }
    if(err){
        dentry_put(child);
        path_put(dir);
        return err;
    }
    d_instantiate(child, ni);
    out = Path{dir.mnt, child};
    path_put(dir);
    created = true;
    return 0;
}

// Creeaza un nume nou in directorul parinte al lui path (mkdir / symlink)
int make_node(const Path& base, const char* path, uint32_t mode, const char* target){
    Path dir;
    LastName last;
    Dentry* child;
    int err = parent_and_child(base, path, dir, last, child);
    if(err){
        return err;
    }
    if(!child){
        path_put(dir);
        return -EEXIST;
    }
    Inode* di = dir.dentry->inode;
    Inode* ni = nullptr;
    if(child->inode){
        err = -EEXIST;
    } else if(target){
        err = di->ops && di->ops->symlink ? di->ops->symlink(di, last.name, last.len, target, ni) : -EPERM;
    } else {
        err = di->ops && di->ops->mkdir ? di->ops->mkdir(di, last.name, last.len, S_IFDIR | (mode & 07777), ni)
                                        : -EPERM;
    }
    if(!err){
        d_instantiate(child, ni);
    }
    dentry_put(child);
    path_put(dir);
    return err;
}

// rmdir (dirs = true) sau unlink
int remove_node(const Path& base, const char* path, bool dirs){
    Path dir;
    LastName last;
    Dentry* child;
    int err = parent_and_child(base, path, dir, last, child);
    if(err){
        return err;
    }
    if(!child){
        path_put(dir);
        if(dirs){
            return last.len == 0 ? -EBUSY : dot_name(last) ? -EINVAL : -ENOTEMPTY;
        }
        return -EISDIR;
    }
    Inode* di = dir.dentry->inode;
    Inode* vi = child->inode;
    if(!vi){
        err = -ENOENT;
    } else if(dirs && !S_ISDIR(vi->mode)){
        err = -ENOTDIR;
    } else if(!dirs && S_ISDIR(vi->mode)){
        err = -EISDIR;
    } else if(!dirs && last.trailing_slash){
        err = -ENOTDIR;
    } else if(child->mounted){
        err = -EBUSY;
    } else {
        auto op = dirs ? di->ops ? di->ops->rmdir : nullptr : di->ops ? di->ops->unlink : nullptr;
        err = op ? op(di, last.name, last.len, vi) : -EPERM;
        if(!err){
            d_drop(child);
        }
    }
    dentry_put(child);
    path_put(dir);
    return err;
}

}

Mutex& vfs_mutex(){
    return lock;
}

void vfs_init(){
    kprintf("[OK] vfs\n");
}

int vfs_open(const Path& base, const char* path, int flags, uint32_t mode, File*& out){
    out = nullptr;
    int acc = flags & O_ACCMODE;
    if(acc == O_ACCMODE){
        return -EINVAL;
    }
    MutexGuard g(lock);
    Path p = {};
    bool created = false;
    int err = -EAGAIN;
    if(flags & O_CREAT){
        err = open_create(base, path, flags, mode, p, created);
    }
    if(err == -EAGAIN){
        err = path_lookup(base, path, (flags & O_NOFOLLOW) ? 0 : LOOKUP_FOLLOW, p, nullptr);
    }
    if(err){
        return err;
    }
    Inode* i = p.dentry->inode;
    if(S_ISLNK(i->mode)){
        err = -ELOOP;                               // O_NOFOLLOW pe un symlink
    } else if((p.mnt->sb->flags & MS_RDONLY) && (acc != O_RDONLY || (flags & O_TRUNC))){
        err = -EROFS;
    } else if(S_ISDIR(i->mode) && (acc != O_RDONLY || (flags & O_TRUNC))){
        err = -EISDIR;
    } else if((flags & O_DIRECTORY) && !S_ISDIR(i->mode)){
        err = -ENOTDIR;
    } else if((flags & O_TRUNC) && !created && S_ISREG(i->mode) && acc != O_RDONLY){
        err = i->ops && i->ops->truncate ? i->ops->truncate(i, 0) : -EPERM;
        if(!err){
            i->mtime = i->ctime = vfs_now();
        }
    }
    if(!err){
        out = vfs_file_new(p, (uint32_t)flags & ~(uint32_t)(O_CREAT | O_EXCL | O_TRUNC));
        if(!out){
            err = -ENOMEM;
        }
    }
    if(err){
        path_put(p);
    }
    return err;
}

int vfs_stat(const Path& base, const char* path, bool follow, Stat& st){
    MutexGuard g(lock);
    Path p;
    int err = path_lookup(base, path, follow ? LOOKUP_FOLLOW : 0, p, nullptr);
    if(!err){
        vfs_fill_stat(p.dentry->inode, st);
        path_put(p);
    }
    return err;
}

int vfs_mkdir(const Path& base, const char* path, uint32_t mode){
    MutexGuard g(lock);
    return make_node(base, path, mode, nullptr);
}

int vfs_rmdir(const Path& base, const char* path){
    MutexGuard g(lock);
    return remove_node(base, path, true);
}

int vfs_unlink(const Path& base, const char* path){
    MutexGuard g(lock);
    return remove_node(base, path, false);
}

int vfs_rename(const Path& base, const char* oldpath, const char* newpath){
    MutexGuard g(lock);
    Path od;
    LastName ol;
    Dentry* oc;
    int err = parent_and_child(base, oldpath, od, ol, oc);
    if(err){
        return err;
    }
    if(!oc){
        path_put(od);
        return ol.len == 0 ? -EBUSY : -EINVAL;
    }
    Path nd;
    LastName nl;
    Dentry* nc;
    err = parent_and_child(base, newpath, nd, nl, nc);
    if(err){
        dentry_put(oc);
        path_put(od);
        return err;
    }
    if(!nc){
        err = nl.len == 0 ? -EBUSY : -EINVAL;
    } else if(!oc->inode){
        err = -ENOENT;
    } else if(oc == nc){
        err = 0;                                    // acelasi nume: nimic de facut
    } else if(oc->sb != nc->sb){
        err = -EXDEV;
    } else if(oc->mounted || nc->mounted){
        err = -EBUSY;
    } else if(od.mnt->sb->flags & MS_RDONLY){
        err = -EROFS;
    } else {
        Inode* src = oc->inode;
        Inode* victim = nc->inode && nc->inode != src ? nc->inode : nullptr;
        bool src_dir = S_ISDIR(src->mode);
        if((ol.trailing_slash || nl.trailing_slash) && !src_dir){
            err = -ENOTDIR;
        } else if(victim && src_dir && !S_ISDIR(victim->mode)){
            err = -ENOTDIR;
        } else if(victim && !src_dir && S_ISDIR(victim->mode)){
            err = -EISDIR;
        } else {
            // Un director nu se muta in propriul subarbore
            for(Dentry* d = nd.dentry; src_dir && d && !err; d = d->parent){
                err = d == oc ? -EINVAL : 0;
            }
            Inode* odi = od.dentry->inode;
            Inode* ndi = nd.dentry->inode;
            if(!err){
                err = odi->ops && odi->ops->rename ? odi->ops->rename(odi, ol.name, ol.len, ndi, nl.name, nl.len, src,
                                                                      victim)
                                                   : -EPERM;
            }
            if(!err){
                // Numele vechi si cel nou dispar din cache; cele deja deschise raman valide pana la ultima referinta
                d_drop(oc);
                d_drop(nc);
            }
        }
    }
    dentry_put(nc ? nc : oc);
    if(nc){
        dentry_put(oc);
    }
    path_put(nd);
    path_put(od);
    return err;
}

int vfs_symlink(const Path& base, const char* target, const char* linkpath){
    uint64_t n = 0;
    while(target[n]){
        n++;
    }
    if(n == 0){
        return -ENOENT;
    }
    if(n >= PATH_MAX){
        return -ENAMETOOLONG;
    }
    MutexGuard g(lock);
    return make_node(base, linkpath, 0777, target);
}

int64_t vfs_readlink(const Path& base, const char* path, char* buf, uint64_t size){
    MutexGuard g(lock);
    Path p;
    int err = path_lookup(base, path, 0, p, nullptr);
    if(err){
        return err;
    }
    Inode* i = p.dentry->inode;
    int64_t r = !S_ISLNK(i->mode) ? -EINVAL : i->ops && i->ops->readlink ? i->ops->readlink(i, buf, size) : -EINVAL;
    path_put(p);
    return r;
}

int vfs_chdir(Path& cwd, const char* path){
    MutexGuard g(lock);
    Path p;
    int err = path_lookup(cwd, path, LOOKUP_FOLLOW, p, nullptr);
    if(err){
        return err;
    }
    if(!S_ISDIR(p.dentry->inode->mode)){
        path_put(p);
        return -ENOTDIR;
    }
    path_put(cwd);
    cwd = p;
    return 0;
}

int vfs_getcwd(const Path& cwd, char* buf, uint64_t size){
    MutexGuard g(lock);
    Path p = cwd;
    if(!p.dentry && !mount_root(p)){
        return -ENOENT;
    }
    int r = path_string(p, buf, size);
    if(!cwd.dentry){
        path_put(p);
    }
    return r;
}

int vfs_mount(const Path& base, const char* dev_name, const char* target, const char* fstype, uint32_t flags,
              const char* data){
    MutexGuard g(lock);
    return mount_do(base, dev_name, target, fstype, flags, data);
}

int vfs_umount(const Path& base, const char* target){
    MutexGuard g(lock);
    return umount_do(base, target);
}

void vfs_path_copy(Path& dst, const Path& src){
    MutexGuard g(lock);
    dst = src;
    if(dst.dentry){
        path_get(dst);
    }
}

void vfs_path_release(Path& p){
    if(p.dentry){
        MutexGuard g(lock);
        path_put(p);
    }
}
