#include "fs/vfs/path.h"
#include "fs/vfs/mount.h"
#include "uapi/syscall_nr.h"
#include "lib/string.h"

namespace {

int walk(Path cur, const char* p, unsigned flags, unsigned& links, Path& out, LastName* last);

// ".." : din radacina unui mount se iese in mount point; la radacina arborelui ramane pe loc
void go_up(Path& p){
    while(p.dentry == p.mnt->root){
        if(!p.mnt->parent){
            return;
        }
        follow_up_mount(p);
    }
    Dentry* par = p.dentry->parent;
    dentry_get(par);
    dentry_put(p.dentry);
    p.dentry = par;
}

// Rezolva tinta symlink-ului link (aflat in directorul parent) in out. Bufferul e separat de walk ca sa nu
// creasca stiva la fiecare nivel de recursie
__attribute__((noinline)) int follow_link(const Path& parent, const Path& link, unsigned& links, Path& out){
    char target[PATH_MAX];
    Inode* li = link.dentry->inode;
    if(!li->ops || !li->ops->readlink){
        return -EINVAL;
    }
    int64_t n = li->ops->readlink(li, target, PATH_MAX - 1);
    if(n < 0){
        return (int)n;
    }
    if(n == 0){
        return -ENOENT;
    }
    target[n] = 0;
    Path start = parent;
    path_get(start);
    return walk(start, target, LOOKUP_FOLLOW, links, out, nullptr);
}

// cur: referinta preluata de walk
int walk(Path cur, const char* p, unsigned flags, unsigned& links, Path& out, LastName* last){
    int err;
    if(*p == '/'){
        path_put(cur);
        if(!mount_root(cur)){
            return -ENOENT;
        }
    }
    bool trailing = false;
    for(;;){
        while(*p == '/'){
            p++;
        }
        if(!*p){
            break;
        }
        const char* comp = p;
        while(*p && *p != '/'){
            p++;
        }
        uint32_t len = (uint32_t)(p - comp);
        if(len > NAME_MAX){
            err = -ENAMETOOLONG;
            goto fail;
        }
        {
            const char* q = p;
            while(*q == '/'){
                q++;
            }
            bool is_last = *q == 0;
            trailing = is_last && q != p;

            Inode* ci = cur.dentry->inode;
            if(!ci || !S_ISDIR(ci->mode)){
                err = -ENOTDIR;
                goto fail;
            }
            if(is_last && (flags & LOOKUP_PARENT)){
                memcpy(last->name, comp, len);
                last->name[len] = 0;
                last->len = len;
                last->trailing_slash = trailing;
                out = cur;
                return 0;
            }
            if(len == 1 && comp[0] == '.'){
                continue;
            }
            if(len == 2 && comp[0] == '.' && comp[1] == '.'){
                go_up(cur);
                continue;
            }
            Dentry* d;
            err = d_lookup(cur.dentry, comp, len, d);
            if(err){
                goto fail;
            }
            if(!d->inode){
                dentry_put(d);
                err = -ENOENT;
                goto fail;
            }
            Path next = {cur.mnt, d};
            follow_mounts(next);
            if(S_ISLNK(next.dentry->inode->mode) && (!is_last || (flags & LOOKUP_FOLLOW) || trailing)){
                if(++links > MAX_SYMLINKS){
                    path_put(next);
                    err = -ELOOP;
                    goto fail;
                }
                Path resolved;
                err = follow_link(cur, next, links, resolved);
                path_put(next);
                if(err){
                    goto fail;
                }
                path_put(cur);
                cur = resolved;
            } else {
                path_put(cur);
                cur = next;
            }
        }
    }
    if(trailing && !S_ISDIR(cur.dentry->inode->mode)){
        err = -ENOTDIR;
        goto fail;
    }
    if(flags & LOOKUP_PARENT){
        // Calea fara componente: "/" sau "//"
        last->name[0] = 0;
        last->len = 0;
        last->trailing_slash = false;
    }
    out = cur;
    return 0;
fail:
    path_put(cur);
    return err;
}

}

int path_lookup(const Path& base, const char* name, unsigned flags, Path& out, LastName* last){
    if(!*name){
        return -ENOENT;
    }
    Path start = base;
    if(start.dentry){
        path_get(start);
    } else if(!mount_root(start)){
        return -ENOENT;
    }
    unsigned links = 0;
    return walk(start, name, flags, links, out, last);
}

void path_get(const Path& p){
    dentry_get(p.dentry);
}

void path_put(Path& p){
    if(p.dentry){
        dentry_put(p.dentry);
    }
    p.dentry = nullptr;
    p.mnt = nullptr;
}

int path_string(const Path& p, char* buf, uint64_t size){
    if(p.dentry->flags & D_DEAD){
        return -ENOENT;
    }
    char tmp[PATH_MAX];
    int pos = PATH_MAX - 1;
    tmp[pos] = 0;
    Path cur = p;
    for(;;){
        while(cur.dentry == cur.mnt->root){
            if(!cur.mnt->parent){
                goto done;
            }
            cur.dentry = cur.mnt->mountpoint;
            cur.mnt = cur.mnt->parent;
        }
        {
            int len = (int)cur.dentry->name_len;
            if(pos < len + 1){
                return -ENAMETOOLONG;
            }
            pos -= len;
            memcpy(tmp + pos, cur.dentry->name, len);
            tmp[--pos] = '/';
            cur.dentry = cur.dentry->parent;
        }
    }
done:
    if(pos == PATH_MAX - 1){
        tmp[--pos] = '/';
    }
    uint64_t n = (uint64_t)(PATH_MAX - pos);
    if(n > size){
        return -ERANGE;
    }
    memcpy(buf, tmp + pos, n);
    return (int)n;
}
