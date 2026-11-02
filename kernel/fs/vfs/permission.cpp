#include "fs/vfs/permission.h"
#include "fs/vfs/vfs.h"
#include "uapi/syscall_nr.h"

int inode_permission(const Cred& c, const Inode* i, int mask){
    mask &= MAY_READ | MAY_WRITE | MAY_EXEC;
    if(cred_is_root(c)){
        // root: orice, dar un fisier se executa doar daca are macar un bit x
        return !(mask & MAY_EXEC) || S_ISDIR(i->mode) || (i->mode & 0111) ? 0 : -EACCES;
    }
    uint32_t bits = c.euid == i->uid ? i->mode >> 6 : cred_in_group(c, i->gid) ? i->mode >> 3 : i->mode;
    return (mask & ~bits & 7) ? -EACCES : 0;
}

int may_create(const Cred& c, const Inode* dir){
    return inode_permission(c, dir, MAY_WRITE | MAY_EXEC);
}

int may_delete(const Cred& c, const Inode* dir, const Inode* victim){
    int err = inode_permission(c, dir, MAY_WRITE | MAY_EXEC);
    if(err){
        return err;
    }
    if((dir->mode & S_ISVTX) && !cred_is_root(c) && c.euid != victim->uid && c.euid != dir->uid){
        return -EPERM;
    }
    return 0;
}

namespace {

// Pune mode/uid/gid si le da fs-ului; la eroare valorile vechi revin
int apply(Inode* i, uint32_t mode, uint32_t uid, uint32_t gid){
    if(!i->ops || !i->ops->setattr){
        return -EPERM;
    }
    uint32_t om = i->mode, ou = i->uid, og = i->gid;
    Timespec oc = i->ctime;
    i->mode = mode;
    i->uid = uid;
    i->gid = gid;
    i->ctime = vfs_now();
    int err = i->ops->setattr(i);
    if(err){
        i->mode = om;
        i->uid = ou;
        i->gid = og;
        i->ctime = oc;
    }
    return err;
}

}

void inode_init_owner(const Cred& c, const Inode* dir, Inode* ni){
    if(!ni->ops || !ni->ops->setattr){
        return;
    }
    bool inherit = (dir->mode & S_ISGID) != 0;
    uint32_t gid = inherit ? dir->gid : c.egid;
    uint32_t mode = ni->mode;
    if(inherit && S_ISDIR(mode)){
        mode |= S_ISGID;
    }
    if(ni->uid != c.euid || ni->gid != gid || ni->mode != mode){
        apply(ni, mode, c.euid, gid);           // o eroare (ext4 fara spatiu in jurnal) lasa proprietarul fs-ului
    }
}

int inode_chmod(Inode* i, uint32_t mode){
    const Cred& c = cred_current();
    if(i->sb->flags & MS_RDONLY){
        return -EROFS;
    }
    if(!cred_is_root(c) && c.euid != i->uid){
        return -EPERM;
    }
    mode &= 07777;
    // Ca pe Linux: cine nu e in grupul fisierului nu poate pune setgid (ar castiga grupul la exec)
    if(!cred_is_root(c) && !S_ISDIR(i->mode) && !cred_in_group(c, i->gid)){
        mode &= ~(uint32_t)S_ISGID;
    }
    return apply(i, (i->mode & S_IFMT) | mode, i->uid, i->gid);
}

int inode_chown(Inode* i, uint32_t uid, uint32_t gid){
    const Cred& c = cred_current();
    if(i->sb->flags & MS_RDONLY){
        return -EROFS;
    }
    uint32_t nu = uid == CRED_NONE ? i->uid : uid;
    uint32_t ng = gid == CRED_NONE ? i->gid : gid;
    if(!cred_is_root(c)){
        // Fara root: doar proprietarul, uid-ul nu se schimba, grupul doar spre unul din grupurile lui
        if(c.euid != i->uid || nu != i->uid || (ng != i->gid && !cred_in_group(c, ng))){
            return -EPERM;
        }
    }
    uint32_t mode = i->mode;
    if(!S_ISDIR(mode) && !S_ISLNK(mode) && (uid != CRED_NONE || gid != CRED_NONE)){
        mode &= ~(uint32_t)S_ISUID;            // un fisier setuid care isi schimba proprietarul pierde bitul
        if(mode & 0010){
            mode &= ~(uint32_t)S_ISGID;
        }
    }
    return apply(i, mode, nu, ng);
}
