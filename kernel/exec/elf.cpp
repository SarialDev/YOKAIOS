#include "exec/elf.h"
#include "uapi/syscall_nr.h"
#include "boot/multiboot2.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "proc/process.h"
#include "irqlock.h"

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
const uint16_t EM_X86_64 = 62;
const uint32_t PT_LOAD = 1;
const uint32_t PF_X = 1;
const uint32_t PF_W = 2;
const uint16_t MAX_PHNUM = 64;
const int MAX_ARGS = 32;
const uint64_t MAX_ARG_BYTES = 4096;

struct UserStart {
    uint64_t rip;
    uint64_t rsp;
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

const BootModule* find_module(const char* name){
    for(uint32_t i = 0; i < boot_info.module_count; i++){
        const char* a = boot_info.modules[i].name;
        const char* b = name;
        while(*a && *a == *b){
            a++;
            b++;
        }
        if(*a == *b){
            return &boot_info.modules[i];
        }
    }
    return nullptr;
}

void user_thread(void* arg){
    UserStart s = *(UserStart*)arg;
    kfree(arg);
    enter_user(s.rip, s.rsp);
}

}

int64_t elf_load(AddressSpace& as, const uint8_t* data, uint64_t size, ElfImage& img){
    if(size < sizeof(Elf64Header)){
        return -EINVAL;
    }
    const Elf64Header* h = (const Elf64Header*)data;
    if(h->ident[0] != 0x7F || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' ||
       h->ident[4] != ELFCLASS64 || h->ident[5] != ELFDATA2LSB || h->ident[6] != EV_CURRENT ||
       h->type != ET_EXEC || h->machine != EM_X86_64 || h->version != EV_CURRENT ||
       h->phentsize != sizeof(Elf64Phdr) || h->phnum == 0 || h->phnum > MAX_PHNUM ||
       h->phoff > size || size - h->phoff < (uint64_t)h->phnum * sizeof(Elf64Phdr)){
        return -EINVAL;
    }
    const Elf64Phdr* ph = (const Elf64Phdr*)(data + h->phoff);
    bool entry_ok = false;
    img.phdr = 0;
    for(uint16_t i = 0; i < h->phnum; i++){
        const Elf64Phdr& p = ph[i];
        if(p.type != PT_LOAD || p.memsz == 0){
            continue;
        }
        if(p.filesz > p.memsz || p.offset > size || size - p.offset < p.filesz ||
           !user_range(p.vaddr, p.memsz) || (p.vaddr & (PAGE_4K - 1)) != (p.offset & (PAGE_4K - 1))){
            return -EINVAL;
        }
        uint64_t flags = (p.flags & PF_W ? PTE_RW : 0) | (p.flags & PF_X ? 0 : PTE_NX);
        for(uint64_t v = page_down(p.vaddr); v < page_up(p.vaddr + p.memsz); v += PAGE_4K){
            if(!map_user_page(as, v, flags)){
                return -EFAULT;
            }
        }
        if(!copy_to_as(as, p.vaddr, data + p.offset, p.filesz)){
            return -EFAULT;
        }
        if((p.flags & PF_X) && h->entry >= p.vaddr && h->entry < p.vaddr + p.memsz){
            entry_ok = true;
        }
        if(h->phoff >= p.offset && h->phoff + h->phnum * sizeof(Elf64Phdr) <= p.offset + p.filesz){
            img.phdr = p.vaddr + (h->phoff - p.offset);
        }
    }
    if(!entry_ok){
        return -EINVAL;
    }
    img.entry = h->entry;
    img.phent = h->phentsize;
    img.phnum = h->phnum;
    return 0;
}

uint64_t elf_setup_stack(AddressSpace& as, const ElfImage& img, const char* const* argv, const char* const* envp){
    int argc = count_strings(argv);
    int envc = count_strings(envp);
    if(argc > MAX_ARGS || envc > MAX_ARGS){
        return 0;
    }
    for(uint64_t v = USER_STACK_TOP - USER_STACK_SIZE; v < USER_STACK_TOP; v += PAGE_4K){
        if(!map_user_page(as, v, PTE_RW | PTE_NX)){
            return 0;
        }
    }

    // Sirurile sus, apoi (in jos) auxv, envp, argv si argc
    uint64_t sp = USER_STACK_TOP;
    uint64_t argv_user[MAX_ARGS];
    uint64_t envp_user[MAX_ARGS];
    for(int i = 0; i < envc + argc; i++){
        const char* s = i < envc ? envp[i] : argv[i - envc];
        uint64_t len = str_len(s) + 1;
        if(USER_STACK_TOP - sp + len > MAX_ARG_BYTES){
            return 0;
        }
        sp -= len;
        copy_to_as(as, sp, s, len);
        (i < envc ? envp_user[i] : argv_user[i - envc]) = sp;
    }
    sp &= ~15UL;

    const uint64_t aux[][2] = {
        {AT_PHDR, img.phdr}, {AT_PHENT, img.phent}, {AT_PHNUM, img.phnum},
        {AT_PAGESZ, PAGE_4K}, {AT_ENTRY, img.entry}, {AT_NULL, 0},
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

// Spatiu de adrese nou cu modulul incarcat si stiva pregatita; nullptr si -errno in err la eroare
AddressSpace* load_module(const char* name, const char* const* argv, const char* const* envp, UserStart& start,
                          int64_t& err){
    const BootModule* m = find_module(name);
    if(!m){
        err = -ENOENT;
        return nullptr;
    }
    AddressSpace* as = (AddressSpace*)kmalloc(sizeof(AddressSpace));
    if(!as){
        err = -ENOMEM;
        return nullptr;
    }
    *as = as_create();
    ElfImage img;
    err = as->pml4_phys ? elf_load(*as, (const uint8_t*)phys_to_virt(m->start), m->end - m->start, img) : -ENOMEM;
    uint64_t rsp = err ? 0 : elf_setup_stack(*as, img, argv, envp);
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
    return as;
}

}

uint64_t exec_spawn_module(const char* name, const char* const* argv, const char* const* envp){
    UserStart* start = (UserStart*)kmalloc(sizeof(UserStart));
    int64_t err = -ENOMEM;
    AddressSpace* as = start ? load_module(name, argv, envp, *start, err) : nullptr;
    Task* t = as ? task_create_kernel(name, user_thread, start) : nullptr;
    Process* p = nullptr;
    if(t){
        t->as = as;
        p = process_create(t, nullptr);
    }
    if(!p){
        kprintf("exec: \"%s\" nu a putut fi incarcat (%ld)\n", name, err ? err : (int64_t)-ENOMEM);
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
    uint64_t pid = p->pid;
    sched_add(t);
    return pid;
}

int64_t exec_replace_module(const char* name, const char* const* argv, const char* const* envp, void* release){
    UserStart start;
    int64_t err;
    AddressSpace* as = load_module(name, argv, envp, start, err);
    if(!as){
        return err;
    }
    // De aici nu mai exista cale de intoarcere: imaginea veche dispare
    Task* t = task_current();
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
    int i = 0;
    for(; i < TASK_NAME_LEN - 1 && name[i]; i++){
        t->name[i] = name[i];
    }
    t->name[i] = 0;
    if(t->proc){
        process_set_name(t->proc, name);
    }
    kfree(release);
    enter_user(start.rip, start.rsp);
}
