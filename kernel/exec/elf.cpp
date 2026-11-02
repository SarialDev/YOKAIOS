#include "exec/elf.h"
#include "proc/caps.h"
#include "arch/x86_64/fpu.h"
#include "uapi/syscall_nr.h"
#include "boot/multiboot2.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "mm/vma.h"
#include "irqlock.h"
#include "fs/vfs/vfs.h"
#include "uapi/fs.h"
#include "lib/entropy.h"

namespace {

struct Elf64Header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};
static_assert(sizeof(Elf64Header) == 64, "header ELF64 are 64 bytes");

struct Elf64Phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};
static_assert(sizeof(Elf64Phdr) == 56, "program header ELF64 are 56 bytes");

const uint8_t ELFCLASS64 = 2;
const uint8_t ELFDATA2LSB = 1;
const uint8_t EV_CURRENT = 1;
const uint16_t ET_EXEC = 2;
const uint16_t ET_DYN = 3;
const uint16_t EM_X86_64 = 62;
const uint32_t PT_LOAD = 1;
const uint32_t PT_DYNAMIC = 2;
const uint32_t PT_INTERP = 3;
const int64_t DT_NULL = 0;
const int64_t DT_NEEDED = 1;
const int64_t DT_PLTRELSZ = 2;
const int64_t DT_RELA = 7;
const int64_t DT_RELASZ = 8;
const int64_t DT_RELAENT = 9;
const int64_t DT_REL = 17;
const int64_t DT_RELSZ = 18;
const int64_t DT_RELRSZ = 35;
const uint32_t R_X86_64_NONE = 0;
const uint32_t R_X86_64_RELATIVE = 8;
const uint32_t PF_X = 1;
const uint32_t PF_W = 2;
const uint16_t MAX_PHNUM = 64;
const int MAX_ARGS = 32;
const uint64_t MAX_ARG_BYTES = 4096;
const uint64_t MAX_EXEC_FILE = 4UL << 20;

struct Elf64Dyn {
    int64_t tag;
    uint64_t val;
};

struct Elf64Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};
static_assert(sizeof(Elf64Rela) == 24, "Elf64_Rela are 24 bytes");

struct UserStart {
    uint64_t rip;
    uint64_t rsp;
    uint64_t brk;
    uint64_t stack_top;
    uint64_t mmap_base;
    VmaList vmas;               // zonele noului proces (se muta in Process inainte de pornire)
};

uint64_t page_down(uint64_t v){
    return v & ~(PAGE_4K - 1);
}

uint64_t page_up(uint64_t v){
    return (v + PAGE_4K - 1) & ~(PAGE_4K - 1);
}

// [start, start + len) incape in jumatatea user, fara overflow
bool user_range(uint64_t start, uint64_t len){
    return start + len >= start && start + len <= USER_TOP;
}

// Pagina user la virt, alocata si zero daca lipseste; flagurile se combina cu ce exista deja
bool map_user_page(AddressSpace& as, uint64_t virt, uint64_t flags){
    uint64_t e = vmm_entry(as, virt);
    if(e){
        if(!(e & PTE_USER)){
            return false;
        }
        // Doua segmente in aceeasi pagina: RW daca oricare e W, NX doar daca niciunul nu e X
        uint64_t merged = ((e | flags) & PTE_RW) | (e & flags & PTE_NX);
        return vmm_map(as, virt, e & PTE_ADDR_MASK, merged | PTE_USER);
    }
    uint64_t phys = pmm_alloc_page();
    if(!phys){
        return false;
    }
    memset(phys_to_virt(phys), 0, PAGE_4K);
    if(!vmm_map(as, virt, phys, flags | PTE_USER)){
        pmm_free_page(phys);
        return false;
    }
    return true;
}

// Copiaza in memoria user deja mapata a lui as, prin direct map (as nu trebuie sa fie activ)
bool copy_to_as(AddressSpace& as, uint64_t virt, const void* src, uint64_t len){
    const uint8_t* s = (const uint8_t*)src;
    while(len){
        uint64_t phys = vmm_translate(as, virt);
        if(!phys){
            return false;
        }
        uint64_t n = PAGE_4K - (virt & (PAGE_4K - 1));
        if(n > len){
            n = len;
        }
        memcpy(phys_to_virt(phys), s, n);
        virt += n;
        s += n;
        len -= n;
    }
    return true;
}

uint64_t str_len(const char* s){
    uint64_t n = 0;
    while(s[n]){
        n++;
    }
    return n;
}

int count_strings(const char* const* v){
    int n = 0;
    while(v && v[n]){
        n++;
    }
    return n;
}

// Numele fisierului din cale (dupa ultimul '/')
const char* base_name(const char* path){
    const char* b = path;
    for(const char* p = path; *p; p++){
        if(*p == '/' && p[1]){
            b = p + 1;
        }
    }
    return b;
}

void user_thread(void* arg){
    UserStart s = *(UserStart*)arg;
    kfree(arg);
    enter_user(s.rip, s.rsp);
}

}

namespace {

// Offsetul in fisier al lui [vaddr, vaddr + len) (adrese din fisier, fara bias), daca e in partea din fisier a unui
// PT_LOAD; false altfel
bool file_range(const Elf64Phdr* ph, uint16_t phnum, uint64_t vaddr, uint64_t len, uint64_t& off){
    for(uint16_t i = 0; i < phnum; i++){
        const Elf64Phdr& p = ph[i];
        if(p.type == PT_LOAD && vaddr >= p.vaddr && len <= p.filesz && vaddr - p.vaddr <= p.filesz - len){
            off = p.offset + (vaddr - p.vaddr);
            return true;
        }
    }
    return false;
}

// static-pie: tabelul din PT_DYNAMIC poate avea doar relocari R_X86_64_RELATIVE (*(bias + offset) = bias + addend).
// Fara biblioteci (DT_NEEDED), fara REL / RELR / PLT. -EINVAL daca nu se poate, -EFAULT daca o tinta nu e mapata
int64_t apply_relocs(AddressSpace& as, const uint8_t* data, uint64_t size, const Elf64Phdr* ph, uint16_t phnum,
                     uint64_t bias, uint64_t& count){
    count = 0;
    const Elf64Phdr* dynp = nullptr;
    for(uint16_t i = 0; i < phnum; i++){
        if(ph[i].type == PT_DYNAMIC){
            dynp = &ph[i];
        }
    }
    if(!dynp){
        return 0;                   // ET_DYN fara relocari (cod complet relativ la RIP)
    }
    if(dynp->offset > size || size - dynp->offset < dynp->filesz){
        return -EINVAL;
    }
    uint64_t rela = 0, relasz = 0, relaent = sizeof(Elf64Rela);
    const Elf64Dyn* d = (const Elf64Dyn*)(data + dynp->offset);
    for(uint64_t i = 0; i < dynp->filesz / sizeof(Elf64Dyn) && d[i].tag != DT_NULL; i++){
        switch(d[i].tag){
        case DT_RELA: rela = d[i].val; break;
        case DT_RELASZ: relasz = d[i].val; break;
        case DT_RELAENT: relaent = d[i].val; break;
        case DT_NEEDED:
            return -EINVAL;
        case DT_REL: case DT_RELSZ: case DT_PLTRELSZ: case DT_RELRSZ:
            if(d[i].val){
                return -EINVAL;
            }
            break;
        default: break;
        }
    }
    if(!relasz){
        return 0;
    }
    uint64_t off;
    if(relaent != sizeof(Elf64Rela) || relasz % sizeof(Elf64Rela) || !file_range(ph, phnum, rela, relasz, off)){
        return -EINVAL;
    }
    const Elf64Rela* r = (const Elf64Rela*)(data + off);
    for(uint64_t i = 0; i < relasz / sizeof(Elf64Rela); i++){
        uint32_t type = (uint32_t)r[i].info;
        if(type == R_X86_64_NONE){
            continue;
        }
        if(type != R_X86_64_RELATIVE || !user_range(r[i].offset + bias, 8)){
            return -EINVAL;
        }
        uint64_t v = bias + (uint64_t)r[i].addend;
        if(!copy_to_as(as, r[i].offset + bias, &v, 8)){
            return -EFAULT;
        }
        count++;
    }
    return 0;
}

}

int64_t elf_load(AddressSpace& as, const uint8_t* data, uint64_t size, ElfImage& img, uint64_t pie_base){
    if(size < sizeof(Elf64Header)){
        return -EINVAL;
    }
    const Elf64Header* h = (const Elf64Header*)data;
    if(h->ident[0] != 0x7F || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' ||
       h->ident[4] != ELFCLASS64 || h->ident[5] != ELFDATA2LSB || h->ident[6] != EV_CURRENT ||
       (h->type != ET_EXEC && h->type != ET_DYN) || h->machine != EM_X86_64 || h->version != EV_CURRENT ||
       h->phentsize != sizeof(Elf64Phdr) || h->phnum == 0 || h->phnum > MAX_PHNUM ||
       h->phoff > size || size - h->phoff < (uint64_t)h->phnum * sizeof(Elf64Phdr)){
        return -EINVAL;
    }
    const Elf64Phdr* ph = (const Elf64Phdr*)(data + h->phoff);
    // ET_DYN: tot fisierul se muta cu bias, astfel incat cel mai mic segment sa inceapa la pie_base
    uint64_t bias = 0;
    if(h->type == ET_DYN){
        uint64_t lo = ~0UL;
        for(uint16_t i = 0; i < h->phnum; i++){
            if(ph[i].type == PT_INTERP){
                return -EINVAL;             // are nevoie de un dynamic loader
            }
            if(ph[i].type == PT_LOAD && ph[i].memsz && page_down(ph[i].vaddr) < lo){
                lo = page_down(ph[i].vaddr);
            }
        }
        if(lo == ~0UL || (pie_base & (PAGE_4K - 1)) || lo > pie_base){
            return -EINVAL;
        }
        bias = pie_base - lo;
    }
    bool entry_ok = false;
    img.phdr = 0;
    img.end = 0;
    img.nseg = 0;
    img.bias = bias;
    img.nrelocs = 0;
    for(uint16_t i = 0; i < h->phnum; i++){
        const Elf64Phdr& p = ph[i];
        if(p.type != PT_LOAD || p.memsz == 0){
            continue;
        }
        uint64_t va = p.vaddr + bias;
        if(p.filesz > p.memsz || p.offset > size || size - p.offset < p.filesz || va < p.vaddr ||
           !user_range(va, p.memsz) || (p.vaddr & (PAGE_4K - 1)) != (p.offset & (PAGE_4K - 1))){
            return -EINVAL;
        }
        if(img.nseg == ELF_MAX_SEGS){
            return -EINVAL;
        }
        uint64_t flags = (p.flags & PF_W ? PTE_RW : 0) | (p.flags & PF_X ? 0 : PTE_NX);
        img.seg[img.nseg++] = ElfSeg{page_down(va), page_up(va + p.memsz),
                                     PROT_READ | (p.flags & PF_W ? PROT_WRITE : 0u) | (p.flags & PF_X ? PROT_EXEC : 0u)};
        for(uint64_t v = page_down(va); v < page_up(va + p.memsz); v += PAGE_4K){
            if(!map_user_page(as, v, flags)){
                return -EFAULT;
            }
        }
        if(!copy_to_as(as, va, data + p.offset, p.filesz)){
            return -EFAULT;
        }
        if(page_up(va + p.memsz) > img.end){
            img.end = page_up(va + p.memsz);
        }
        if((p.flags & PF_X) && h->entry >= p.vaddr && h->entry < p.vaddr + p.memsz){
            entry_ok = true;
        }
        if(h->phoff >= p.offset && h->phoff + h->phnum * sizeof(Elf64Phdr) <= p.offset + p.filesz){
            img.phdr = va + (h->phoff - p.offset);
        }
    }
    if(!entry_ok){
        return -EINVAL;
    }
    if(h->type == ET_DYN){
        int64_t err = apply_relocs(as, data, size, ph, h->phnum, bias, img.nrelocs);
        if(err){
            return err;
        }
    }
    img.entry = h->entry + bias;
    img.phent = h->phentsize;
    img.phnum = h->phnum;
    return 0;
}

bool aslr_enabled(){
    static int state = -1;
    if(state < 0){
        state = cmdline_has("norandmaps") ? 0 : 1;
    }
    return state == 1;
}

UserLayout aslr_layout(){
    UserLayout l = {ET_DYN_BASE, USER_STACK_TOP, 0, MMAP_BASE, 0};
    if(!aslr_enabled()){
        return l;
    }
    uint64_t r1 = entropy_u64(), r2 = entropy_u64();
    l.pie_base = ET_DYN_BASE + (r1 % ASLR_PIE_PAGES) * PAGE_4K;
    l.mmap_base = MMAP_BASE + ((r1 >> 32) % ASLR_MMAP_PAGES) * PAGE_4K;
    l.stack_top = USER_STACK_TOP - (r2 % ASLR_STACK_PAGES) * PAGE_4K;
    l.sp_offset = ((r2 >> 24) & 0xFF) * 16;
    l.brk_gap = ((r2 >> 40) % ASLR_BRK_PAGES) * PAGE_4K;
    return l;
}

uint64_t elf_setup_stack(AddressSpace& as, const ElfImage& img, const char* const* argv, const char* const* envp,
                         uint64_t stack_top, uint64_t sp_offset){
    int argc = count_strings(argv);
    int envc = count_strings(envp);
    if(argc > MAX_ARGS || envc > MAX_ARGS){
        return 0;
    }
    if(sp_offset >= PAGE_4K || (stack_top & (PAGE_4K - 1)) || stack_top > USER_STACK_TOP){
        return 0;
    }
    for(uint64_t v = stack_top - USER_STACK_SIZE; v < stack_top; v += PAGE_4K){
        if(!map_user_page(as, v, PTE_RW | PTE_NX)){
            return 0;
        }
    }

    // Sirurile sus, apoi (in jos) auxv, envp, argv si argc
    const uint64_t top = stack_top - sp_offset;
    uint64_t sp = top;
    uint64_t argv_user[MAX_ARGS];
    uint64_t envp_user[MAX_ARGS];
    for(int i = 0; i < envc + argc; i++){
        const char* s = i < envc ? envp[i] : argv[i - envc];
        uint64_t len = str_len(s) + 1;
        if(top - sp + len > MAX_ARG_BYTES){
            return 0;
        }
        sp -= len;
        copy_to_as(as, sp, s, len);
        (i < envc ? envp_user[i] : argv_user[i - envc]) = sp;
    }
    sp &= ~15UL;

    const uint64_t aux[][2] = {
        {AT_PHDR, img.phdr}, {AT_PHENT, img.phent}, {AT_PHNUM, img.phnum},
        {AT_PAGESZ, PAGE_4K}, {AT_BASE, 0}, {AT_ENTRY, img.entry}, {AT_NULL, 0},
    };
    const uint64_t aux_count = sizeof(aux) / sizeof(aux[0]);
    uint64_t words = 1 + (argc + 1) + (envc + 1) + 2 * aux_count;
    sp -= words * 8;
    sp &= ~15UL;                                // rsp % 16 == 0 la _start, ca in ABI-ul System V

    uint64_t w = sp;
    uint64_t v = (uint64_t)argc;
    auto put = [&](uint64_t x){
        copy_to_as(as, w, &x, 8);
        w += 8;
    };
    put(v);
    for(int i = 0; i < argc; i++){
        put(argv_user[i]);
    }
    put(0);
    for(int i = 0; i < envc; i++){
        put(envp_user[i]);
    }
    put(0);
    for(uint64_t i = 0; i < aux_count; i++){
        put(aux[i][0]);
        put(aux[i][1]);
    }
    return sp;
}

namespace {

// VMA-urile unui proces proaspat incarcat: segmentele ELF (deja mapate), stiva (restul vine la cerere) si trambulina
int build_vmas(VmaList& l, const ElfImage& img, uint64_t stack_top){
    ElfSeg seg[ELF_MAX_SEGS];
    int n = img.nseg;
    for(int i = 0; i < n; i++){
        seg[i] = img.seg[i];
    }
    for(int i = 1; i < n; i++){                 // sortare prin insertie dupa start
        ElfSeg x = seg[i];
        int j = i - 1;
        for(; j >= 0 && seg[j].start > x.start; j--){
            seg[j + 1] = seg[j];
        }
        seg[j + 1] = x;
    }
    for(int i = 0; i < n; i++){
        if(i > 0 && seg[i].start < seg[i - 1].end){
            // Doua segmente in aceeasi pagina: pagina ramane la primul, cu drepturile unite
            seg[i - 1].prot |= seg[i].prot;
            seg[i].start = seg[i - 1].end;
        }
        if(seg[i].start >= seg[i].end){
            continue;
        }
        if(vma_insert(l, seg[i].start, seg[i].end, seg[i].prot, MAP_PRIVATE | MAP_ANONYMOUS, nullptr, 0)){
            return -ENOMEM;
        }
    }
    if(vma_insert(l, stack_top - USER_STACK_MAX, stack_top, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | VMA_STACK, nullptr, 0) ||
       vma_insert(l, SIGTRAMP_ADDR, SIGTRAMP_ADDR + PAGE_4K, PROT_READ | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS | VMA_SYSTEM, nullptr, 0)){
        return -ENOMEM;
    }
    return 0;
}

// Identitatea fisierului executat, pentru setuid/setgid (proc/cred.h)
struct ExecId {
    uint32_t mode, uid, gid;
    bool nosuid;
    bool cap_found;                 // fisierul are politica de capabilitati (proc/caps.h)
    uint64_t cap_mask;
};

// Identitatea si capabilitatile procesului dupa execve: setuid/setgid, trecerea root <-> utilizator pentru capabilitati,
// apoi plafonul din politica pachetului (capabilitatile nu cresc niciodata la exec)
void apply_exec_id(Process* p, const ExecId& id){
    Cred before = p->cred;
    cred_exec(p->cred, id.mode, id.uid, id.gid, id.nosuid);
    caps_on_setid(p->caps, before, p->cred);
    if(id.cap_found){
        caps_limit(p->caps, id.cap_mask);
    }
}

// Continutul fisierului de executat: drept x (VFS_O_EXEC), nu r; kmalloc, nullptr si -errno in err
uint8_t* read_file(const Path& base, const char* path, uint64_t& size, ExecId& id, int64_t& err){
    File* f;
    err = vfs_open(base, path, O_RDONLY | VFS_O_EXEC, 0, f);
    if(err){
        return nullptr;
    }
    uint8_t* buf = nullptr;
    id = ExecId{f->inode->mode, f->inode->uid, f->inode->gid, (f->inode->sb->flags & MS_NOSUID) != 0, false, 0};
    if(f->inode->size > MAX_EXEC_FILE || f->inode->size == 0){
        err = f->inode->size ? -ENOMEM : -ENOEXEC;
    } else {
        size = f->inode->size;
        buf = (uint8_t*)kmalloc(size);
        err = buf ? 0 : -ENOMEM;
    }
    for(uint64_t got = 0; !err && got < size;){
        int64_t n = f->ops->read(f, buf + got, size - got, false);
        if(n <= 0){
            err = n < 0 ? n : -EIO;
        } else {
            got += (uint64_t)n;
        }
    }
    file_put(f);
    if(err){
        kfree(buf);
        buf = nullptr;
    }
    return buf;
}

// Spatiu de adrese nou cu programul de la path incarcat si stiva pregatita; nullptr si -errno in err la eroare
AddressSpace* load_path(const Path& base, const char* path, const char* const* argv, const char* const* envp,
                        UserStart& start, ExecId& id, int64_t& err){
    uint64_t size = 0;
    uint8_t* image = read_file(base, path, size, id, err);
    if(!image){
        return nullptr;
    }
    caps_exec_policy(base, path, id.cap_found, id.cap_mask);
    AddressSpace* as = (AddressSpace*)kmalloc(sizeof(AddressSpace));
    if(!as){
        kfree(image);
        err = -ENOMEM;
        return nullptr;
    }
    *as = as_create();
    ElfImage img;
    UserLayout lay = aslr_layout();
    err = as->pml4_phys ? elf_load(*as, image, size, img, lay.pie_base) : -ENOMEM;
    kfree(image);
    if(err == -EINVAL){
        err = -ENOEXEC;
    }
    uint64_t rsp = err ? 0 : elf_setup_stack(*as, img, argv, envp, lay.stack_top, lay.sp_offset);
    start.vmas = VmaList{};
    if(rsp && (!signal_map_trampoline(*as) || build_vmas(start.vmas, img, lay.stack_top))){
        vma_list_free(start.vmas);
        rsp = 0;
        err = -ENOMEM;
    }
    if(!rsp){
        err = err ? err : -E2BIG;
        if(as->pml4_phys){
            as_destroy(*as);
        }
        kfree(as);
        return nullptr;
    }
    start.rip = img.entry;
    start.rsp = rsp;
    start.brk = img.end + lay.brk_gap;
    start.stack_top = lay.stack_top;
    start.mmap_base = lay.mmap_base;
    return as;
}

}

uint64_t exec_spawn(const char* path, const char* const* argv, const char* const* envp){
    UserStart* start = (UserStart*)kmalloc(sizeof(UserStart));
    if(start){
        start->vmas = VmaList{};
    }
    int64_t err = -ENOMEM;
    ExecId id = {};
    AddressSpace* as = start ? load_path(Path{}, path, argv, envp, *start, id, err) : nullptr;
    Task* t = as ? task_create_kernel(base_name(path), user_thread, start) : nullptr;
    Process* p = nullptr;
    if(t){
        t->as = as;
        p = process_create(t, nullptr);
    }
    if(p){
        apply_exec_id(p, id);
        p->brk_base = p->brk_cur = start->brk;
        p->stack_top = start->stack_top;
        p->mmap_base = start->mmap_base;
        p->vmas = start->vmas;
        start->vmas = VmaList{};
    }
    if(!p){
        kprintf("exec: \"%s\" nu a putut fi incarcat (%ld)\n", path, err ? err : (int64_t)-ENOMEM);
        if(start){
            vma_list_free(start->vmas);
        }
        if(t){
            task_destroy(t);
        } else if(as){
            as_destroy(*as);
            kfree(as);
        }
        kfree(start);
        return 0;
    }
    fd_init_console(p->fds);
    process_set_cmdline(p, argv);
    uint64_t pid = p->pid;
    sched_add(t);
    return pid;
}

int64_t exec_replace(const char* path, const char* const* argv, const char* const* envp, void* release){
    UserStart start;
    int64_t err;
    Process* proc = process_current();
    ExecId id;
    AddressSpace* as = load_path(proc ? proc->cwd : Path{}, path, argv, envp, start, id, err);
    if(!as){
        return err;
    }
    // De aici nu mai exista cale de intoarcere: imaginea veche dispare
    Task* t = task_current();
    if(t->proc){
        vma_exit(t->proc);                      // MAP_SHARED murdar se scrie inapoi cat timp vechiul spatiu e activ
        t->proc->vmas = start.vmas;
        signal_exec(t->proc);
        apply_exec_id(t->proc, id);
        caps_exec_reset_io();
    } else {
        vma_list_free(start.vmas);
    }
    AddressSpace* old = t->as;
    {
        IrqLock lock;
        t->as = as;
        as_switch(*as);
    }
    if(old != &kernel_as){
        as_destroy(*old);
        kfree(old);
    }
    const char* name = base_name(path);
    int i = 0;
    for(; i < TASK_NAME_LEN - 1 && name[i]; i++){
        t->name[i] = name[i];
    }
    t->name[i] = 0;
    if(t->proc){
        t->proc->brk_base = t->proc->brk_cur = start.brk;
        t->proc->stack_top = start.stack_top;
        t->proc->mmap_base = start.mmap_base;
        process_set_name(t->proc, name);
        process_set_cmdline(t->proc, argv);
    }
    kfree(release);
    fpu_reset();
    enter_user(start.rip, start.rsp);
}
