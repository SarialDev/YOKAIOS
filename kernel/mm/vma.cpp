#include "mm/vma.h"
#include "mm/fault.h"
#include "mm/pmm.h"
#include "mm/pageref.h"
#include "mm/kmalloc.h"
#include "proc/process.h"
#include "fs/vfs/inode.h"
#include "exec/elf.h"
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"
#include "sched/sched.h"
#include "lib/string.h"

namespace {

const uint64_t MMAP_MIN = 0x10000;
const uint64_t BRK_MAX = 1UL << 30;             // heap-ul brk: cel mult 1 GiB peste sfarsitul imaginii
// Tot ce e la sau peste trambulina de semnale ramane al kernelului
const uint64_t USER_LIMIT = USER_STACK_TOP;

uint64_t page_up(uint64_t v){
    return (v + PAGE_4K - 1) & ~(PAGE_4K - 1);
}

AddressSpace& cur_as(){
    return *task_current()->as;
}

Vma* new_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, File* file, uint64_t off){
    Vma* v = (Vma*)kzalloc(sizeof(Vma));
    if(!v){
        return nullptr;
    }
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->file = file;
    v->off = off;
    if(file){
        file_get(file);
    }
    return v;
}

void free_vma(Vma* v){
    if(v->file){
        file_put(v->file);
    }
    kfree(v);
}

// PTE-ul (fara adresa) al unei pagini private / partajate noi din zona v
uint64_t pte_flags(uint32_t prot, uint32_t flags){
    uint64_t f = 0;
    if(prot){
        f |= PTE_USER;                          // PROT_NONE: pagina ramane, dar user nu o poate atinge
    }
    if(prot & PROT_WRITE){
        f |= PTE_RW;
    }
    if(!(prot & PROT_EXEC)){
        f |= PTE_NX;
    }
    if(flags & MAP_SHARED){
        f |= PTE_SHM;
    }
    return f;
}

bool range_free(const VmaList& l, uint64_t start, uint64_t end){
    for(Vma* v = l.head; v && v->start < end; v = v->next){
        if(v->end > start){
            return false;
        }
    }
    return true;
}

// Prima gaura de len bytes in [lo, hi), 0 daca nu exista
uint64_t find_gap(const VmaList& l, uint64_t len, uint64_t lo, uint64_t hi){
    uint64_t cur = lo;
    for(Vma* v = l.head; v; v = v->next){
        if(v->end <= cur){
            continue;
        }
        if(v->start >= cur && v->start - cur >= len){
            break;
        }
        if(v->end > cur){
            cur = v->end;
        }
    }
    return cur + len <= hi && cur + len > cur ? cur : 0;
}

void link_sorted(VmaList& l, Vma* nv){
    Vma** pp = &l.head;
    while(*pp && (*pp)->start < nv->start){
        pp = &(*pp)->next;
    }
    nv->next = *pp;
    *pp = nv;
    l.count++;
}

void unlink(VmaList& l, Vma* v){
    Vma** pp = &l.head;
    while(*pp != v){
        pp = &(*pp)->next;
    }
    *pp = v->next;
    l.count--;
}

// Imparte v in [start, at) si [at, end); intoarce partea de sus, nullptr fara memorie
Vma* split(VmaList& l, Vma* v, uint64_t at){
    Vma* hi = new_vma(at, v->end, v->prot, v->flags, v->file, v->off + (at - v->start));
    if(!hi){
        return nullptr;
    }
    v->end = at;
    hi->next = v->next;
    v->next = hi;
    l.count++;
    return hi;
}

// Taie zonele astfel incat start si end sa fie granite de zona; false fara memorie
bool split_range(VmaList& l, uint64_t start, uint64_t end){
    Vma* v = vma_find(l, start);
    if(v && v->start < start && !split(l, v, start)){
        return false;
    }
    v = vma_find(l, end - 1);
    if(v && v->end > end && !split(l, v, end)){
        return false;
    }
    return true;
}

bool shared_file(const Vma* v){
    return (v->flags & MAP_SHARED) && v->file && v->file->inode;
}

// Scrie in fisier paginile murdare (bit D) din [from, to) ale unei zone MAP_SHARED pe fisier
void writeback(AddressSpace& as, Vma* v, uint64_t from, uint64_t to, bool clear){
    if(!shared_file(v)){
        return;
    }
    Inode* ino = v->file->inode;
    if(!ino->ops || !ino->ops->write){
        return;
    }
    for(uint64_t page = from; page < to; page += PAGE_4K){
        uint64_t e = vmm_entry(as, page);
        if(!(e & PTE_PRESENT) || !(e & PTE_DIRTY)){
            continue;
        }
        uint64_t pos = v->off + (page - v->start);
        if(pos >= ino->size){
            continue;
        }
        uint64_t len = ino->size - pos < PAGE_4K ? ino->size - pos : PAGE_4K;
        ino->ops->write(ino, pos, phys_to_virt(e & PTE_ADDR_MASK), len);
        if(clear){
            vmm_map(as, page, e & PTE_ADDR_MASK, (e & ~PTE_ADDR_MASK) & ~PTE_DIRTY);
        }
    }
}

// Pagina lui page din zona v, aduse in memorie (zero sau din fisier)
bool populate(AddressSpace& as, Vma* v, uint64_t page){
    uint64_t phys = pmm_alloc_page();
    if(!phys){
        return false;
    }
    uint8_t* kp = (uint8_t*)phys_to_virt(phys);
    uint64_t got = 0;
    if(v->file && v->file->inode && v->file->inode->ops && v->file->inode->ops->read){
        Inode* ino = v->file->inode;
        uint64_t pos = v->off + (page - v->start);
        while(got < PAGE_4K){
            int64_t n = ino->ops->read(ino, pos + got, kp + got, PAGE_4K - got);
            if(n <= 0){
                break;
            }
            got += (uint64_t)n;
        }
    }
    memset(kp + got, 0, PAGE_4K - got);
    if(!vmm_map(as, page, phys, pte_flags(v->prot, v->flags))){
        pmm_free_page(phys);
        return false;
    }
    return true;
}

// Scriere pe o pagina CoW: copie privata daca mai e partajata, altfel doar devine scriibila
bool cow_break(AddressSpace& as, uint64_t page, uint64_t e){
    uint64_t phys = e & PTE_ADDR_MASK;
    uint64_t flags = (e & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_RW;
    if(!page_shared(phys)){
        return vmm_map(as, page, phys, flags);
    }
    uint64_t copy = pmm_alloc_page();
    if(!copy){
        return false;
    }
    memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_4K);
    if(!vmm_map(as, page, copy, flags)){
        pmm_free_page(copy);
        return false;
    }
    page_put(phys);
    return true;
}

// Drepturile PTE-ului unei pagini deja mapate dupa mprotect
void apply_prot(AddressSpace& as, Vma* v){
    for(uint64_t page = v->start; page < v->end; page += PAGE_4K){
        uint64_t e = vmm_entry(as, page);
        if(!(e & PTE_PRESENT)){
            continue;
        }
        uint64_t phys = e & PTE_ADDR_MASK;
        uint64_t f = e & ~(PTE_ADDR_MASK | PTE_RW | PTE_NX | PTE_USER | PTE_COW);
        if(v->prot){
            f |= PTE_USER;
        }
        if(!(v->prot & PROT_EXEC)){
            f |= PTE_NX;
        }
        f |= e & PTE_COW;
        if(v->prot & PROT_WRITE){
            if((e & PTE_SHM) || (!(e & PTE_COW) && !page_shared(phys))){
                f |= PTE_RW;
            } else {
                f |= PTE_COW;                   // pagina e (sau ar putea fi) partajata: scrierea o sparge
            }
        }
        vmm_map(as, page, phys, f);
    }
}

}

Vma* vma_find(const VmaList& l, uint64_t addr){
    for(Vma* v = l.head; v && v->start <= addr; v = v->next){
        if(addr < v->end){
            return v;
        }
    }
    return nullptr;
}

int vma_insert(VmaList& l, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, File* file, uint64_t off){
    Vma* v = new_vma(start, end, prot, flags, file, off);
    if(!v){
        return -ENOMEM;
    }
    link_sorted(l, v);
    return 0;
}

void vma_list_free(VmaList& l){
    while(l.head){
        Vma* v = l.head;
        l.head = v->next;
        free_vma(v);
    }
    l.count = 0;
}

int vma_list_clone(VmaList& dst, const VmaList& src){
    Vma** tail = &dst.head;
    for(Vma* v = src.head; v; v = v->next){
        Vma* c = new_vma(v->start, v->end, v->prot, v->flags, v->file, v->off);
        if(!c){
            vma_list_free(dst);
            return -ENOMEM;
        }
        *tail = c;
        tail = &c->next;
        dst.count++;
    }
    return 0;
}

void vma_exit(Process* p){
    AddressSpace& as = cur_as();
    for(Vma* v = p->vmas.head; v; v = v->next){
        writeback(as, v, v->start, v->end, false);
    }
    vma_list_free(p->vmas);
}

void vma_unmap_pages(AddressSpace& as, uint64_t from, uint64_t to){
    for(uint64_t page = from; page < to; page += PAGE_4K){
        uint64_t e = vmm_entry(as, page);
        if(e & PTE_PRESENT){
            vmm_unmap(as, page);
            page_put(e & PTE_ADDR_MASK);
        }
    }
}

int vma_munmap(uint64_t addr, uint64_t len){
    Process* p = process_current();
    if(!p || (addr & (PAGE_4K - 1)) || len == 0){
        return -EINVAL;
    }
    len = page_up(len);
    uint64_t end = addr + len;
    if(end < addr || end > USER_LIMIT){
        return -EINVAL;
    }
    VmaList& l = p->vmas;
    if(!split_range(l, addr, end)){
        return -ENOMEM;
    }
    AddressSpace& as = cur_as();
    Vma* v = l.head;
    while(v && v->end <= addr){
        v = v->next;
    }
    while(v && v->start < end){
        Vma* next = v->next;
        writeback(as, v, v->start, v->end, false);
        vma_unmap_pages(as, v->start, v->end);
        unlink(l, v);
        free_vma(v);
        v = next;
    }
    return 0;
}

int vma_mprotect(uint64_t addr, uint64_t len, uint64_t prot){
    Process* p = process_current();
    if(!p || (addr & (PAGE_4K - 1)) || (prot & ~7UL)){
        return -EINVAL;
    }
    len = page_up(len);
    uint64_t end = addr + len;
    if(len == 0){
        return 0;
    }
    if(end < addr || end > USER_LIMIT){
        return -ENOMEM;
    }
    VmaList& l = p->vmas;
    // Tot intervalul trebuie acoperit de zone
    uint64_t cur = addr;
    for(Vma* v = vma_find(l, addr); v && cur < end; v = v->next){
        if(v->start > cur){
            return -ENOMEM;
        }
        if(v->flags & VMA_SYSTEM){
            return -EINVAL;
        }
        if((prot & PROT_WRITE) && shared_file(v) && (v->file->flags & O_ACCMODE) != O_RDWR){
            return -EACCES;
        }
        cur = v->end;
    }
    if(cur < end){
        return -ENOMEM;
    }
    if(!split_range(l, addr, end)){
        return -ENOMEM;
    }
    AddressSpace& as = cur_as();
    for(Vma* v = vma_find(l, addr); v && v->start < end; v = v->next){
        v->prot = (uint32_t)prot;
        apply_prot(as, v);
    }
    return 0;
}

int vma_msync(uint64_t addr, uint64_t len, uint64_t flags){
    Process* p = process_current();
    if(!p || (addr & (PAGE_4K - 1)) || (flags & ~(uint64_t)(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) ||
       ((flags & MS_ASYNC) && (flags & MS_SYNC))){
        return -EINVAL;
    }
    uint64_t end = addr + page_up(len);
    if(end < addr || end > USER_LIMIT){
        return -ENOMEM;
    }
    AddressSpace& as = cur_as();
    uint64_t cur = addr;
    for(Vma* v = vma_find(p->vmas, addr); v && cur < end; v = v->next){
        if(v->start > cur){
            return -ENOMEM;
        }
        uint64_t from = cur > v->start ? cur : v->start;
        uint64_t to = end < v->end ? end : v->end;
        writeback(as, v, from, to, true);
        cur = v->end;
    }
    return cur < end ? -ENOMEM : 0;
}

int64_t vma_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off){
    Process* p = process_current();
    if(!p || len == 0 || (prot & ~7UL) || (off & (PAGE_4K - 1))){
        return -EINVAL;
    }
    bool shared = flags & MAP_SHARED;
    bool priv = flags & MAP_PRIVATE;
    if(shared == priv){
        return -EINVAL;
    }
    if(len > USER_LIMIT){
        return -ENOMEM;
    }
    len = page_up(len);
    File* f = nullptr;
    if(!(flags & MAP_ANONYMOUS)){
        f = fd_get(p->fds, fd);
        if(!f){
            return -EBADF;
        }
        if(!f->inode || !S_ISREG(f->inode->mode) || !f->inode->ops || !f->inode->ops->read){
            return -ENODEV;
        }
        if((f->flags & O_ACCMODE) == O_WRONLY){
            return -EACCES;
        }
        if(shared && (prot & PROT_WRITE) && (f->flags & O_ACCMODE) != O_RDWR){
            return -EACCES;
        }
        if(off + len < off){
            return -EINVAL;
        }
    }
    VmaList& l = p->vmas;
    AddressSpace& as = cur_as();
    uint64_t start = 0;
    if(flags & MAP_FIXED){
        if((addr & (PAGE_4K - 1)) || addr < MMAP_MIN || addr + len < addr || addr + len > USER_LIMIT){
            return -EINVAL;
        }
        int err = vma_munmap(addr, len);
        if(err){
            return err;
        }
        start = addr;
    } else {
        if(addr && !(addr & (PAGE_4K - 1)) && addr >= MMAP_MIN && addr + len > addr && addr + len <= MMAP_END &&
           range_free(l, addr, addr + len)){
            start = addr;
        } else {
            start = find_gap(l, len, MMAP_BASE, MMAP_END);
        }
        if(!start){
            return -ENOMEM;
        }
    }
    uint32_t vflags = (uint32_t)flags & (MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS);
    // Zone anonime vecine cu aceleasi drepturi se unesc (mmap in bucla nu umple lista)
    Vma* prev = nullptr;
    for(Vma* v = l.head; v && v->start < start; v = v->next){
        prev = v;
    }
    bool merged = false;
    uint64_t old_end = 0;
    if(!f && prev && prev->end == start && !prev->file && prev->prot == prot && prev->flags == vflags){
        old_end = prev->end;
        prev->end = start + len;
        merged = true;
    } else if(vma_insert(l, start, start + len, (uint32_t)prot, vflags, f, off)){
        return -ENOMEM;
    }
    // MAP_SHARED anonim: paginile se aloca acum, ca fork sa le poata imparti (nu exista obiect de memorie partajata)
    if(shared && !f){
        Vma tmp = {start, start + len, (uint32_t)prot, vflags, nullptr, 0, nullptr};
        for(uint64_t page = start; page < start + len; page += PAGE_4K){
            if(!populate(as, &tmp, page)){
                vma_unmap_pages(as, start, page);
                if(merged){
                    prev->end = old_end;
                } else {
                    Vma* nv = vma_find(l, start);
                    unlink(l, nv);
                    free_vma(nv);
                }
                return -ENOMEM;
            }
        }
    }
    return (int64_t)start;
}

uint64_t vma_brk(uint64_t addr){
    Process* p = process_current();
    if(addr < p->brk_base || addr - p->brk_base > BRK_MAX){
        return p->brk_cur;
    }
    VmaList& l = p->vmas;
    AddressSpace& as = cur_as();
    uint64_t old_top = page_up(p->brk_cur);
    uint64_t new_top = page_up(addr);
    Vma* h = nullptr;
    for(Vma* v = l.head; v; v = v->next){
        if(v->flags & VMA_HEAP){
            h = v;
            break;
        }
    }
    if(new_top > old_top){
        if(!range_free(l, old_top, new_top)){
            return p->brk_cur;
        }
        if(h){
            h->end = new_top;
        } else if(vma_insert(l, p->brk_base, new_top, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | VMA_HEAP,
                             nullptr, 0)){
            return p->brk_cur;
        }
    } else if(new_top < old_top && h){
        vma_unmap_pages(as, new_top, old_top);
        if(new_top <= h->start){
            unlink(l, h);
            free_vma(h);
        } else {
            h->end = new_top;
        }
    }
    p->brk_cur = addr;
    return addr;
}

bool vma_fault(AddressSpace& as, uint64_t addr, uint64_t err){
    if(addr >= USER_TOP){
        return false;
    }
    Task* t = task_current();
    Process* p = t ? t->proc : nullptr;
    if(!p){
        return false;
    }
    Vma* v = vma_find(p->vmas, addr);
    if(!v){
        return false;
    }
    bool write = err & PF_ERR_WRITE;
    bool fetch = err & PF_ERR_FETCH;
    if((write && !(v->prot & PROT_WRITE)) || (fetch && !(v->prot & PROT_EXEC)) || !v->prot){
        return false;
    }
    uint64_t page = addr & ~(PAGE_4K - 1);
    uint64_t e = vmm_entry(as, page);
    if(e & PTE_PRESENT){
        if(!(err & PF_ERR_PRESENT)){
            return true;                        // intre timp a aparut (TLB vechi)
        }
        if(write && !(e & PTE_RW) && (e & PTE_COW)){
            return cow_break(as, page, e);
        }
        return false;
    }
    return populate(as, v, page);
}

bool vma_fault_in(uint64_t addr, bool write){
    AddressSpace& as = cur_as();
    uint64_t e = vmm_entry(as, addr);
    uint64_t err = PF_ERR_USER | (write ? PF_ERR_WRITE : 0) | ((e & PTE_PRESENT) ? PF_ERR_PRESENT : 0);
    return vma_fault(as, addr, err);
}
