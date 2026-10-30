#include "fs/pseudofs.h"
#include "fs/vfs/vfs.h"
#include "drivers/fb/fb.h"
#include "hal/hal.h"
#include "hal/console.h"
#include "hal/block.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

// Nodurile sunt derivate din HAL la fiecare lookup/readdir: ce e inregistrat apare singur (fara udev).
// Blocurile iau numele cu care driverul le-a inregistrat (sda, nvme0n1)
enum class Kind : uint8_t { Null, Zero, Random, Urandom, Console, Serial, Fb, Block };

struct DevDesc {
    const char* name;
    Kind kind;
    int idx;                        // indexul in registrul HAL (Serial, Fb, Block)
};

struct DevNode {
    Inode* ino;
    DevNode* next;
    Kind kind;
    int idx;
};

struct DevFs {
    DevNode* all;                   // sub lock-ul VFS (lookup, readdir, kill)
};

const uint32_t BLOCK_MAX_RUN = 128;     // blocuri citite/scrise dintr-o data
const char SERIAL_CONSOLE[] = "serial";

bool have_rdrand = false;
uint64_t fallback_state = 0;
Mutex block_lock("devfs-block");

extern const InodeOps root_ops;
extern const InodeOps dev_ops;

bool name_is(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

uint32_t name_len(const char* s){
    uint32_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

int serial_index(){
    for(int i = 0; i < hal_count(HalKind::Console); i++){
        if(name_is(hal_name(HalKind::Console, i), SERIAL_CONSOLE)){
            return i;
        }
    }
    return -1;
}

int fb_index(){
    Framebuffer* real = fb_device();
    for(int i = 0; real && i < hal_count(HalKind::Framebuffer); i++){
        if(hal_get(HalKind::Framebuffer, i) == real){
            return i;
        }
    }
    return -1;
}

// Intrarea numarul idx din /dev (null, zero, random, urandom, console, ttyS0, fb0, apoi blocurile)
bool entry_at(uint64_t idx, DevDesc& d){
    static const struct { const char* name; Kind kind; } fixed[] = {
        {"null", Kind::Null}, {"zero", Kind::Zero}, {"random", Kind::Random}, {"urandom", Kind::Urandom},
        {"console", Kind::Console},
    };
    const uint64_t nfixed = sizeof(fixed) / sizeof(fixed[0]);
    if(idx < nfixed){
        d = DevDesc{fixed[idx].name, fixed[idx].kind, 0};
        return true;
    }
    idx -= nfixed;
    int ser = serial_index();
    if(ser >= 0){
        if(idx == 0){
            d = DevDesc{"ttyS0", Kind::Serial, ser};
            return true;
        }
        idx--;
    }
    int fb = fb_index();
    if(fb >= 0){
        if(idx == 0){
            d = DevDesc{hal_name(HalKind::Framebuffer, fb), Kind::Fb, fb};
            return true;
        }
        idx--;
    }
    if(idx < (uint64_t)hal_count(HalKind::Block)){
        d = DevDesc{hal_name(HalKind::Block, (int)idx), Kind::Block, (int)idx};
        return true;
    }
    return false;
}

DevNode* node_of(Inode* i){
    return (DevNode*)i->priv;
}

// ---- RDRAND, cu fallback pe TSC (QEMU TCG nu are RDRAND) ----

bool rdrand64(uint64_t& out){
    for(int i = 0; i < 10; i++){
        uint64_t v;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if(ok){
            out = v;
            return true;
        }
    }
    return false;
}

uint64_t tsc_mix(){
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = __atomic_add_fetch(&fallback_state, 0x9E3779B97F4A7C15UL, __ATOMIC_RELAXED) ^ ((uint64_t)hi << 32 | lo);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9UL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBUL;
    return x ^ (x >> 31);
}

void random_fill(uint8_t* buf, uint64_t len){
    for(uint64_t i = 0; i < len; i += 8){
        uint64_t v;
        if(!have_rdrand || !rdrand64(v)){
            v = tsc_mix();
        }
        uint64_t n = len - i < 8 ? len - i : 8;
        memcpy(buf + i, &v, n);
    }
}

// ---- fb0: pixeli de 32 bit (formatul framebuffer-ului), linii fara padding de pitch ----

int64_t fb_rw(uint64_t pos, void* rbuf, const void* wbuf, uint64_t len){
    uint64_t w = fb_width();
    uint64_t size = w * fb_height() * 4;
    if(pos >= size){
        return rbuf ? 0 : -ENOSPC;
    }
    if(pos % 4){
        return -EINVAL;
    }
    if(len > size - pos){
        len = size - pos;
    }
    uint64_t px = len / 4;
    if(px == 0){
        return -EINVAL;
    }
    for(uint64_t i = 0; i < px; i++){
        uint64_t p = pos / 4 + i;
        int x = (int)(p % w);
        int y = (int)(p / w);
        if(rbuf){
            ((uint32_t*)rbuf)[i] = fb_getpixel(x, y);
        } else {
            fb_putpixel(x, y, ((const uint32_t*)wbuf)[i]);
        }
    }
    return (int64_t)(px * 4);
}

// ---- dispozitive bloc, la octet: sectoarele partiale se citesc, se modifica si se scriu inapoi ----

int64_t block_rw(BlockDevice* b, uint64_t pos, void* rbuf, const void* wbuf, uint64_t len){
    uint64_t bs = b->block_size();
    uint64_t cap = bs * b->block_count();
    if(bs == 0 || len == 0){
        return bs ? 0 : -EIO;
    }
    if(pos >= cap){
        return rbuf ? 0 : -ENOSPC;
    }
    if(len > cap - pos){
        len = cap - pos;
    }
    uint8_t* bounce = nullptr;
    uint64_t done = 0;
    int64_t result = 0;
    if(!rbuf){
        block_lock.lock();
    }
    while(done < len){
        uint64_t p = pos + done;
        uint64_t off = p % bs;
        uint64_t left = len - done;
        int r;
        uint64_t step;
        if(off == 0 && left >= bs){
            uint64_t run = left / bs;
            if(run > BLOCK_MAX_RUN){
                run = BLOCK_MAX_RUN;
            }
            step = run * bs;
            r = rbuf ? b->read(p / bs, (uint32_t)run, (uint8_t*)rbuf + done)
                     : b->write(p / bs, (uint32_t)run, (const uint8_t*)wbuf + done);
        } else {
            if(!bounce){
                bounce = (uint8_t*)kmalloc(bs);
                if(!bounce){
                    result = -ENOMEM;
                    break;
                }
            }
            step = bs - off < left ? bs - off : left;
            r = b->read(p / bs, 1, bounce);
            if(r == 0){
                if(rbuf){
                    memcpy((uint8_t*)rbuf + done, bounce + off, step);
                } else {
                    memcpy(bounce + off, (const uint8_t*)wbuf + done, step);
                    r = b->write(p / bs, 1, bounce);
                }
            }
        }
        if(r != 0){
            result = -EIO;
            break;
        }
        done += step;
    }
    if(!rbuf){
        if(done){
            b->flush();
        }
        block_lock.unlock();
    }
    kfree(bounce);
    return done ? (int64_t)done : result;
}

// ---- operatiile pe noduri ----

int64_t dev_read(Inode* i, uint64_t pos, void* buf, uint64_t len){
    DevNode* n = node_of(i);
    switch(n->kind){
    case Kind::Null:
    case Kind::Console:
    case Kind::Serial:
        return 0;                       // fara tty: consola nu primeste input prin read
    case Kind::Zero:
        memset(buf, 0, len);
        return (int64_t)len;
    case Kind::Random:
    case Kind::Urandom:
        random_fill((uint8_t*)buf, len);
        return (int64_t)len;
    case Kind::Fb:
        return fb_rw(pos, buf, nullptr, len);
    case Kind::Block: {
        BlockDevice* b = hal_block(n->idx);
        return b ? block_rw(b, pos, buf, nullptr, len) : -EIO;
    }
    }
    return -EINVAL;
}

int64_t dev_write(Inode* i, uint64_t pos, const void* buf, uint64_t len){
    DevNode* n = node_of(i);
    switch(n->kind){
    case Kind::Null:
    case Kind::Zero:
    case Kind::Random:
    case Kind::Urandom:
        return (int64_t)len;
    case Kind::Console:
        for(uint64_t k = 0; k < len; k++){
            hal_console_putc(((const char*)buf)[k]);
        }
        return (int64_t)len;
    case Kind::Serial: {
        Console* c = hal_console(n->idx);
        for(uint64_t k = 0; c && k < len; k++){
            c->putc(((const char*)buf)[k]);
        }
        return c ? (int64_t)len : -EIO;
    }
    case Kind::Fb:
        return fb_rw(pos, nullptr, buf, len);
    case Kind::Block: {
        BlockDevice* b = hal_block(n->idx);
        return b ? block_rw(b, pos, nullptr, buf, len) : -EIO;
    }
    }
    return -EINVAL;
}

DevNode* node_get(SuperBlock* sb, const DevDesc& d){
    DevFs* fs = (DevFs*)sb->priv;
    for(DevNode* n = fs->all; n; n = n->next){
        if(n->kind == d.kind && n->idx == d.idx){
            return n;
        }
    }
    DevNode* n = (DevNode*)kzalloc(sizeof(DevNode));
    if(!n){
        return nullptr;
    }
    uint32_t mode = S_IFCHR | 0666;
    uint64_t size = 0;
    if(d.kind == Kind::Fb){
        mode = S_IFCHR | 0660;
        size = (uint64_t)fb_width() * fb_height() * 4;
    } else if(d.kind == Kind::Block){
        BlockDevice* b = hal_block(d.idx);
        mode = S_IFBLK | 0660;
        size = b ? (uint64_t)b->block_size() * b->block_count() : 0;
    }
    uint64_t ino = 2 + (uint64_t)d.kind * 256 + (uint64_t)d.idx;
    Inode* i = inode_alloc(sb, ino, mode, &dev_ops);
    if(!i){
        kfree(n);
        return nullptr;
    }
    i->priv = n;
    i->size = size;
    n->ino = i;
    n->kind = d.kind;
    n->idx = d.idx;
    n->next = fs->all;
    fs->all = n;
    return n;
}

int root_lookup(Inode* dir, const char* name, uint32_t len, Inode*& out){
    DevDesc d;
    for(uint64_t k = 0; entry_at(k, d); k++){
        if(name_len(d.name) == len && memcmp(d.name, name, len) == 0){
            DevNode* n = node_get(dir->sb, d);
            if(!n){
                return -ENOMEM;
            }
            out = n->ino;
            return 0;
        }
    }
    return -ENOENT;
}

int root_readdir(Inode* dir, uint64_t index, DirEntry& out){
    DevDesc d;
    if(!entry_at(index, d)){
        return 0;
    }
    DevNode* n = node_get(dir->sb, d);
    if(!n){
        return -ENOMEM;
    }
    uint32_t len = name_len(d.name);
    out.ino = n->ino->ino;
    out.type = S_ISBLK(n->ino->mode) ? DT_BLK : DT_CHR;
    out.name_len = (uint8_t)len;
    memcpy(out.name, d.name, len);
    out.name[len] = 0;
    return 1;
}

// Doar lookup si readdir: create/mkdir/unlink dau -EPERM (nu exista udev)
const InodeOps root_ops = {root_lookup, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           root_readdir, nullptr, nullptr};
const InodeOps dev_ops = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, dev_read, dev_write, nullptr,
                          nullptr, nullptr, nullptr};

void dev_release(Inode*){
}

void dev_kill(SuperBlock* sb){
    DevFs* fs = (DevFs*)sb->priv;
    while(fs->all){
        DevNode* n = fs->all;
        fs->all = n->next;
        inode_free(n->ino);
        kfree(n);
    }
    kfree(fs);
}

const SuperBlockOps dev_sb_ops = {dev_release, dev_kill};

int dev_mount(FileSystemType*, SuperBlock* sb, const char*, const char*){
    DevFs* fs = (DevFs*)kzalloc(sizeof(DevFs));
    if(!fs){
        return -ENOMEM;
    }
    Inode* root = inode_alloc(sb, 1, S_IFDIR | 0755, &root_ops);
    if(!root){
        kfree(fs);
        return -ENOMEM;
    }
    root->nlink = 2;
    sb->ops = &dev_sb_ops;
    sb->priv = fs;
    sb->root_inode = root;
    // Radacina nu e in lista all: o elibereaza kill-ul prin lista ca un nod fara date
    DevNode* rn = (DevNode*)kzalloc(sizeof(DevNode));
    if(!rn){
        inode_free(root);
        kfree(fs);
        return -ENOMEM;
    }
    rn->ino = root;
    rn->kind = Kind::Null;
    rn->idx = -1;
    rn->next = nullptr;
    fs->all = rn;
    return 0;
}

FileSystemType devfs_type = {"devfs", dev_mount, nullptr};

}

void devfs_init(){
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    have_rdrand = (c & (1u << 30)) != 0;
    fs_register(&devfs_type);
}

bool devfs_has_rdrand(){
    return have_rdrand;
}
