#include "syscall/fs.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "proc/process.h"
#include "fs/vfs/vfs.h"
#include "mm/kmalloc.h"
#include "lib/string.h"

namespace {

const uint64_t FSTYPE_MAX = 32;
const uint64_t DATA_MAX = 64;
const uint64_t DIRENT_BUF = 4096;

// Sirul user in dst (cel mult max bytes cu terminator): 0 sau -errno
int path_from_user(char* dst, uint64_t user, uint64_t max = PATH_MAX){
    int64_t n = copy_string_from_user(dst, user, max);
    return n == -E2BIG ? -ENAMETOOLONG : n < 0 ? (int)n : 0;
}

int stat_to_user(uint64_t user, const Stat& st){
    return user_access_ok(user, sizeof(st), true) && copy_to_user(user, &st, sizeof(st)) ? 0 : -EFAULT;
}

int64_t stat_path(uint64_t path, uint64_t user, bool follow){
    Process* p = process_current();
    char kp[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    Stat st;
    if(!err){
        err = vfs_stat(p->cwd, kp, follow, st);
    }
    return err ? err : stat_to_user(user, st);
}

}

int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kp[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    if(err){
        return err;
    }
    File* f;
    err = vfs_open(p->cwd, kp, (int)flags, (uint32_t)mode, f);
    if(err){
        return err;
    }
    int fd = fd_install(p->fds, f);
    if(fd < 0){
        file_put(f);
    }
    return fd;
}

int64_t sys_stat(uint64_t path, uint64_t st, uint64_t, uint64_t, uint64_t, uint64_t){
    return stat_path(path, st, true);
}

int64_t sys_lstat(uint64_t path, uint64_t st, uint64_t, uint64_t, uint64_t, uint64_t){
    return stat_path(path, st, false);
}

int64_t sys_fstat(uint64_t fd, uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    File* f = p ? fd_get(p->fds, (int64_t)fd) : nullptr;
    if(!f){
        return -EBADF;
    }
    Stat st;
    if(f->inode){
        vfs_fill_stat(f->inode, st);
    } else {
        // Consola si pipe-uri: fara inode
        memset(&st, 0, sizeof(st));
        st.st_mode = (f == console_file() ? S_IFCHR : S_IFIFO) | 0600;
        st.st_nlink = 1;
        st.st_blksize = 4096;
    }
    return stat_to_user(user, st);
}

int64_t sys_lseek(uint64_t fd, uint64_t off, uint64_t whence, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    File* f = p ? fd_get(p->fds, (int64_t)fd) : nullptr;
    if(!f){
        return -EBADF;
    }
    return f->ops->lseek ? f->ops->lseek(f, (int64_t)off, (int)whence) : -ESPIPE;
}

int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    char kb[PATH_MAX];
    int n = vfs_getcwd(p->cwd, kb, size < PATH_MAX ? size : PATH_MAX);
    if(n < 0){
        return n;
    }
    return user_access_ok(buf, (uint64_t)n, true) && copy_to_user(buf, kb, (uint64_t)n) ? n : -EFAULT;
}

int64_t sys_chdir(uint64_t path, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kp[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    return err ? err : vfs_chdir(p->cwd, kp);
}

int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kp[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    return err ? err : vfs_mkdir(p->cwd, kp, (uint32_t)mode);
}

int64_t sys_rmdir(uint64_t path, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kp[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    return err ? err : vfs_rmdir(p->cwd, kp);
}

int64_t sys_unlink(uint64_t path, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kp[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    return err ? err : vfs_unlink(p->cwd, kp);
}

int64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kt[PATH_MAX];
    char kl[PATH_MAX];
    int err = p ? path_from_user(kt, target) : -EINVAL;
    if(!err){
        err = path_from_user(kl, linkpath);
    }
    return err ? err : vfs_symlink(p->cwd, kt, kl);
}

int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t size, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kp[PATH_MAX];
    char kb[PATH_MAX];
    int err = p ? path_from_user(kp, path) : -EINVAL;
    if(err){
        return err;
    }
    int64_t n = vfs_readlink(p->cwd, kp, kb, size < PATH_MAX ? size : PATH_MAX);
    if(n <= 0){
        return n;
    }
    return user_access_ok(buf, (uint64_t)n, true) && copy_to_user(buf, kb, (uint64_t)n) ? n : -EFAULT;
}

int64_t sys_mount(uint64_t source, uint64_t target, uint64_t fstype, uint64_t flags, uint64_t data, uint64_t){
    Process* p = process_current();
    char ks[PATH_MAX] = "";
    char kt[PATH_MAX];
    char kf[FSTYPE_MAX];
    char kd[DATA_MAX] = "";
    int err = p ? path_from_user(kt, target) : -EINVAL;
    if(!err && source){
        err = path_from_user(ks, source);
    }
    if(!err){
        err = path_from_user(kf, fstype, FSTYPE_MAX);
    }
    if(!err && data){
        err = path_from_user(kd, data, DATA_MAX);
    }
    return err ? err : vfs_mount(p->cwd, ks, kt, kf, (uint32_t)flags, data ? kd : nullptr);
}

int64_t sys_umount2(uint64_t target, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    char kt[PATH_MAX];
    int err = p ? path_from_user(kt, target) : -EINVAL;
    return err ? err : vfs_umount(p->cwd, kt);
}

int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    File* f = p ? fd_get(p->fds, (int64_t)fd) : nullptr;
    if(!f){
        return -EBADF;
    }
    if(!f->ops->getdents){
        return -ENOTDIR;
    }
    if(!user_access_ok(buf, count, true)){
        return -EFAULT;
    }
    uint64_t cap = count < DIRENT_BUF ? count : DIRENT_BUF;
    void* kb = kmalloc(cap ? cap : 1);
    if(!kb){
        return -ENOMEM;
    }
    int64_t n = f->ops->getdents(f, kb, cap);
    if(n > 0 && !copy_to_user(buf, kb, (uint64_t)n)){
        n = -EFAULT;
    }
    kfree(kb);
    return n;
}
