#include "fs/vfs/mount.h"
#include "uapi/syscall_nr.h"
#include "mm/kmalloc.h"
#include "lib/string.h"

namespace {

Mount* mounts = nullptr;            // toate, cele mai noi primele
Mount* root_mount = nullptr;
uint64_t next_dev = 1;
uint64_t live_mounts = 0;

Mount* mount_find(Mount* parent, Dentry* mp){
    for(Mount* m = mounts; m; m = m->next){
        if(m->parent == parent && m->mountpoint == mp){
            return m;
        }
    }
    return nullptr;
}

// Calea e doar "/" (cu oricate slash-uri)
bool is_root_path(const char* s){
    if(*s != '/'){
        return false;
    }
    while(*s == '/'){
        s++;
    }
    return *s == 0;
}

}

bool mount_root(Path& out){
    if(!root_mount){
        return false;
    }
    out = Path{root_mount, root_mount->root};
    dentry_get(out.dentry);
    return true;
}

void follow_mounts(Path& p){
    while(p.dentry->mounted){
        Mount* m = mount_find(p.mnt, p.dentry);
        if(!m){
            break;
        }
        dentry_get(m->root);
        dentry_put(p.dentry);
        p = Path{m, m->root};
    }
}

void follow_up_mount(Path& p){
    Mount* m = p.mnt;
    dentry_get(m->mountpoint);
    dentry_put(p.dentry);
    p = Path{m->parent, m->mountpoint};
}

int mount_do(const Path& base, const char* dev_name, const char* target, const char* fstype, uint32_t flags,
             const char* data){
    FileSystemType* type = fs_find(fstype);
    if(!type || !type->mount){
        return -ENODEV;
    }
    bool first = !root_mount;
    Path tp = {};
    if(first){
        if(!is_root_path(target)){
            return -ENOENT;
        }
    } else {
        int err = path_lookup(base, target, LOOKUP_FOLLOW, tp, nullptr);
        if(err){
            return err;
        }
        if(!S_ISDIR(tp.dentry->inode->mode)){
            path_put(tp);
            return -ENOTDIR;
        }
    }
    SuperBlock* sb = (SuperBlock*)kzalloc(sizeof(SuperBlock));
    Mount* m = (Mount*)kzalloc(sizeof(Mount));
    int err = -ENOMEM;
    if(sb && m){
        sb->type = type;
        sb->dev = next_dev++;
        sb->flags = flags;
        err = type->mount(type, sb, dev_name, data);
        if(!err && (!sb->root_inode || !S_ISDIR(sb->root_inode->mode))){
            if(sb->ops && sb->ops->kill){
                sb->ops->kill(sb);
            }
            err = -EINVAL;
        } else if(!err){
            m->root = d_alloc_root(sb, sb->root_inode);
            if(!m->root){
                if(sb->ops && sb->ops->kill){
                    sb->ops->kill(sb);
                }
                err = -ENOMEM;
            }
        }
    }
    if(err){
        kfree(sb);
        kfree(m);
        path_put(tp);
        return err;
    }
    m->sb = sb;
    for(uint32_t i = 0; dev_name && dev_name[i] && i < sizeof(m->dev_name) - 1; i++){
        m->dev_name[i] = dev_name[i];
    }
    if(!first){
        m->parent = tp.mnt;
        m->mountpoint = tp.dentry;      // preia referinta din tp
        tp.dentry->mounted++;
    }
    m->next = mounts;
    mounts = m;
    if(first){
        root_mount = m;
    }
    live_mounts++;
    return 0;
}

int umount_do(const Path& base, const char* target){
    Path p;
    int err = path_lookup(base, target, LOOKUP_FOLLOW, p, nullptr);
    if(err){
        return err;
    }
    Mount* m = p.mnt;
    if(p.dentry != m->root){
        path_put(p);
        return -EINVAL;
    }
    if(!m->parent){
        path_put(p);
        return -EBUSY;
    }
    bool busy = false;
    for(Mount* x = mounts; x; x = x->next){
        busy |= x->parent == m;
    }
    for(Dentry* d = m->sb->dentries; d && !busy; d = d->sb_next){
        // Radacina: referinta mount-ului si cea a path_lookup-ului de mai sus
        busy = d_external_refs(d) > (d == m->root ? 2u : 0u);
    }
    if(busy){
        path_put(p);
        return -EBUSY;
    }
    path_put(p);
    Mount** pp = &mounts;
    while(*pp != m){
        pp = &(*pp)->next;
    }
    *pp = m->next;
    live_mounts--;
    m->mountpoint->mounted--;
    dentry_put(m->mountpoint);
    SuperBlock* sb = m->sb;
    d_kill_sb(sb);
    dentry_put(m->root);
    if(sb->ops && sb->ops->kill){
        sb->ops->kill(sb);
    }
    kfree(sb);
    kfree(m);
    return 0;
}

int umount_root_do(){
    Mount* m = root_mount;
    if(!m){
        return -EINVAL;
    }
    bool busy = false;
    for(Mount* x = mounts; x; x = x->next){
        busy |= x->parent == m;
    }
    for(Dentry* d = m->sb->dentries; d && !busy; d = d->sb_next){
        busy = d_external_refs(d) > (d == m->root ? 1u : 0u);
    }
    if(busy){
        return -EBUSY;
    }
    mounts = nullptr;
    root_mount = nullptr;
    live_mounts--;
    SuperBlock* sb = m->sb;
    d_kill_sb(sb);
    dentry_put(m->root);
    if(sb->ops && sb->ops->kill){
        sb->ops->kill(sb);
    }
    kfree(sb);
    kfree(m);
    return 0;
}

uint64_t mount_count(){
    return live_mounts;
}

namespace {

// Calea mount-ului: calea mount point-ului in mount-ul parinte, concatenata la calea parintelui. false daca nu incape
bool mount_path(const Mount* m, char* buf, uint64_t size){
    if(!m->parent){
        if(size < 2){
            return false;
        }
        buf[0] = '/';
        buf[1] = 0;
        return true;
    }
    char tmp[PATH_MAX];
    uint64_t pos = sizeof(tmp) - 1;
    tmp[pos] = 0;
    for(const Dentry* d = m->mountpoint; d->parent; d = d->parent){
        if(pos < d->name_len + 1u){
            return false;
        }
        pos -= d->name_len;
        memcpy(tmp + pos, d->name, d->name_len);
        tmp[--pos] = '/';
    }
    char parent[PATH_MAX];
    if(!mount_path(m->parent, parent, sizeof(parent))){
        return false;
    }
    uint64_t plen = 0;
    while(parent[plen]){
        plen++;
    }
    if(plen == 1){
        plen = 0;
    }
    uint64_t rest = sizeof(tmp) - 1 - pos;
    if(plen + rest + 1 > size){
        return false;
    }
    memcpy(buf, parent, plen);
    memcpy(buf + plen, tmp + pos, rest + 1);
    return true;
}

}

uint64_t mount_snapshot(MountInfo* out, uint64_t max){
    uint64_t n = 0;
    for(Mount* m = mounts; m; m = m->next){
        n++;
    }
    uint64_t filled = 0;
    // Lista are cele mai noi primele: parcurg indicii de la cel mai vechi
    for(uint64_t want = n; want > 0 && filled < max; want--){
        Mount* m = mounts;
        for(uint64_t k = 1; k < want; k++){
            m = m->next;
        }
        MountInfo& e = out[filled];
        memcpy(e.dev_name, m->dev_name, sizeof(e.dev_name));
        if(!mount_path(m, e.path, sizeof(e.path))){
            e.path[0] = '?';
            e.path[1] = 0;
        }
        e.fstype = m->sb->type->name;
        e.flags = m->sb->flags;
        filled++;
    }
    return filled;
}
