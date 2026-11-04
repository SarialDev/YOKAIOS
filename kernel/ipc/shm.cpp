// Memorie partajata POSIX (T10.2): shm_open / shm_unlink / ftruncate + mmap(MAP_SHARED) prin mm/vma.cpp
#include "ipc/shm.h"
#include "sync/mutex.h"
#include "mm/pmm.h"
#include "mm/pageref.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "proc/process.h"
#include "proc/cred.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"
#include "lib/string.h"
#include "fs/vfs/inode.h"

namespace {

struct ShmObj {
    char name[SHM_NAME_MAX];
    uint64_t size;
    uint64_t npages;
    uint64_t* pages;            // adrese fizice; fiecare pagina are referinta obiectului
    uint32_t refs;              // numele (cat e legat) + fiecare File deschis
    bool linked;
    uint32_t uid, gid, mode;
    ShmObj* next;
};

Mutex shm_mutex("shm");
ShmObj* objects = nullptr;
uint64_t live = 0;

uint32_t slen(const char* s){
    uint32_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

ShmObj* find(const char* name){
    for(ShmObj* o = objects; o; o = o->next){
        if(o->linked && slen(o->name) == slen(name) && !memcmp(o->name, name, slen(name))){
            return o;
        }
    }
    return nullptr;
}

void unlink_obj(ShmObj* o){
    ShmObj** pp = &objects;
    while(*pp && *pp != o){
        pp = &(*pp)->next;
    }
    if(*pp){
        *pp = o->next;
    }
    o->linked = false;
}

// Cu shm_mutex luat: paginile peste n se elibereaza (referinta obiectului)
void shrink(ShmObj* o, uint64_t n){
    while(o->npages > n){
        page_put(o->pages[--o->npages]);
    }
}

void put(ShmObj* o){
    if(--o->refs){
        return;
    }
    shrink(o, 0);
    kfree(o->pages);
    kfree(o);
    live--;
}

// Cu shm_mutex luat
int resize(ShmObj* o, uint64_t size){
    if(size > SHM_SIZE_MAX){
        return -EFBIG;
    }
    uint64_t need = (size + PAGE_4K - 1) / PAGE_4K;
    if(need < o->npages){
        shrink(o, need);
    } else if(need > o->npages){
        uint64_t* np = (uint64_t*)kmalloc((need ? need : 1) * sizeof(uint64_t));
        if(!np){
            return -ENOMEM;
        }
        if(o->npages){
            memcpy(np, o->pages, o->npages * sizeof(uint64_t));
        }
        kfree(o->pages);
        o->pages = np;
        while(o->npages < need){
            uint64_t phys = pmm_alloc_page();
            if(!phys){
                return -ENOMEM;
            }
            memset(phys_to_virt(phys), 0, PAGE_4K);
            o->pages[o->npages++] = phys;
        }
    }
    // O micsorare urmata de marire lasa zero in coada ultimei pagini (ca pe Linux)
    if(size < o->size && (size & (PAGE_4K - 1)) && need){
        uint8_t* last = (uint8_t*)phys_to_virt(o->pages[need - 1]);
        memset(last + (size & (PAGE_4K - 1)), 0, PAGE_4K - (size & (PAGE_4K - 1)));
    }
    o->size = size;
    return 0;
}

ShmObj* obj_of(const File* f);

int64_t shm_read(File* f, void* buf, uint64_t len, bool){
    ShmObj* o = obj_of(f);
    MutexGuard g(shm_mutex);
    if(f->pos >= o->size){
        return 0;
    }
    uint64_t n = len < o->size - f->pos ? len : o->size - f->pos;
    uint64_t done = 0;
    while(done < n){
        uint64_t pos = f->pos + done;
        uint64_t in = pos & (PAGE_4K - 1);
        uint64_t k = PAGE_4K - in < n - done ? PAGE_4K - in : n - done;
        memcpy((uint8_t*)buf + done, (uint8_t*)phys_to_virt(o->pages[pos / PAGE_4K]) + in, k);
        done += k;
    }
    f->pos += n;
    return (int64_t)n;
}

// Scrierea nu mareste obiectul (ca pe Linux tmpfs ar face-o; aici dimensiunea o schimba doar ftruncate)
int64_t shm_write(File* f, const void* buf, uint64_t len, bool){
    ShmObj* o = obj_of(f);
    MutexGuard g(shm_mutex);
    if(f->pos >= o->size){
        return -EFBIG;
    }
    uint64_t n = len < o->size - f->pos ? len : o->size - f->pos;
    uint64_t done = 0;
    while(done < n){
        uint64_t pos = f->pos + done;
        uint64_t in = pos & (PAGE_4K - 1);
        uint64_t k = PAGE_4K - in < n - done ? PAGE_4K - in : n - done;
        memcpy((uint8_t*)phys_to_virt(o->pages[pos / PAGE_4K]) + in, (const uint8_t*)buf + done, k);
        done += k;
    }
    f->pos += n;
    return (int64_t)n;
}

int64_t shm_lseek(File* f, int64_t off, int whence){
    ShmObj* o = obj_of(f);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : whence == SEEK_END ? (int64_t)o->size : -1;
    if(base < 0 || base + off < 0){
        return -EINVAL;
    }
    f->pos = (uint64_t)(base + off);
    return (int64_t)f->pos;
}

void shm_close(File* f){
    MutexGuard g(shm_mutex);
    put(obj_of(f));
}

const FileOps shm_ops = {shm_read, shm_write, shm_close, shm_lseek, nullptr, nullptr};

ShmObj* obj_of(const File* f){
    return (ShmObj*)f->priv;
}

// "/nume": un singur '/', la inceput, si cel putin un caracter dupa
int name_from_user(uint64_t user, char* out){
    int64_t n = copy_string_from_user(out, user, SHM_NAME_MAX);
    if(n < 0){
        return n == -E2BIG ? -ENAMETOOLONG : (int)n;
    }
    if(n < 2 || out[0] != '/'){
        return -EINVAL;
    }
    for(int64_t i = 1; i < n; i++){
        if(out[i] == '/'){
            return -EINVAL;
        }
    }
    return 0;
}

// Drepturile ca la un fisier: proprietar / grup / altii; root trece
bool may_open(const ShmObj* o, uint32_t acc){
    const Cred& c = cred_current();
    if(cred_is_root(c)){
        return true;
    }
    uint32_t bits = o->mode & 07;
    if(c.euid == o->uid){
        bits = (o->mode >> 6) & 07;
    } else if(c.egid == o->gid || cred_in_group(c, o->gid)){
        bits = (o->mode >> 3) & 07;
    }
    uint32_t want = acc == O_RDONLY ? 4 : acc == O_WRONLY ? 2 : 6;
    return (bits & want) == want;
}

}

bool shm_is(const File* f){
    return f && f->ops == &shm_ops;
}

uint64_t shm_size(const File* f){
    MutexGuard g(shm_mutex);
    return obj_of(f)->size;
}

int64_t shm_page(File* f, uint64_t off){
    MutexGuard g(shm_mutex);
    ShmObj* o = obj_of(f);
    if(off & (PAGE_4K - 1) || off / PAGE_4K >= o->npages){
        return -ENXIO;
    }
    return (int64_t)o->pages[off / PAGE_4K];
}

int shm_truncate(File* f, uint64_t size){
    MutexGuard g(shm_mutex);
    return resize(obj_of(f), size);
}

uint64_t shm_count(){
    MutexGuard g(shm_mutex);
    return live;
}

int64_t sys_shm_open(uint64_t uname, uint64_t flags, uint64_t mode, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    char name[SHM_NAME_MAX];
    int err = name_from_user(uname, name);
    if(err){
        return err;
    }
    uint32_t acc = (uint32_t)flags & O_ACCMODE;
    if(acc == O_ACCMODE){
        return -EINVAL;
    }
    File* f = nullptr;
    {
        MutexGuard g(shm_mutex);
        ShmObj* o = find(name);
        if(o && (flags & O_CREAT) && (flags & O_EXCL)){
            return -EEXIST;
        }
        if(!o){
            if(!(flags & O_CREAT)){
                return -ENOENT;
            }
            o = (ShmObj*)kzalloc(sizeof(ShmObj));
            if(!o){
                return -ENOMEM;
            }
            memcpy(o->name, name, slen(name) + 1);
            const Cred& c = cred_current();
            o->uid = c.euid;
            o->gid = c.egid;
            o->mode = (uint32_t)mode & 0777 & ~p->umask;
            o->linked = true;
            o->refs = 1;                        // numele
            o->next = objects;
            objects = o;
            live++;
        } else if(!may_open(o, acc)){
            return -EACCES;
        }
        if((flags & O_TRUNC) && acc != O_RDONLY){
            resize(o, 0);
        }
        f = file_alloc(&shm_ops, o);
        if(!f){
            return -ENOMEM;
        }
        o->refs++;
    }
    f->flags = (uint32_t)flags & (O_ACCMODE | O_NONBLOCK);
    int fd = fd_install(p->fds, f);
    if(fd < 0){
        file_put(f);
    }
    return fd;
}

int64_t sys_shm_unlink(uint64_t uname, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    char name[SHM_NAME_MAX];
    int err = name_from_user(uname, name);
    if(err){
        return err;
    }
    MutexGuard g(shm_mutex);
    ShmObj* o = find(name);
    if(!o){
        return -ENOENT;
    }
    const Cred& c = cred_current();
    if(!cred_is_root(c) && c.euid != o->uid){
        return -EACCES;
    }
    unlink_obj(o);
    put(o);
    return 0;
}

int64_t sys_ftruncate(uint64_t fd, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    File* f = p ? fd_get(p->fds, (int64_t)fd) : nullptr;
    if(!f){
        return -EBADF;
    }
    if((int64_t)len < 0){
        return -EINVAL;
    }
    if((f->flags & O_ACCMODE) == O_RDONLY){
        return -EINVAL;                         // ca pe Linux: fd deschis doar pentru citire
    }
    if(shm_is(f)){
        return shm_truncate(f, len);
    }
    Inode* i = f->inode;
    if(!i || !S_ISREG(i->mode)){
        return -EINVAL;
    }
    return i->ops && i->ops->truncate ? i->ops->truncate(i, len) : -EPERM;
}
