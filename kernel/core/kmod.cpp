#include "core/kmod.h"
#include "core/verify.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "proc/caps.h"
#include "sync/spinlock.h"
#include "sync/mutex.h"
#include "syscall/syscall.h"
#include "uapi/syscall_nr.h"
#include "lib/string.h"
#include "lib/kprintf.h"

namespace {

// ---- ELF64 (doar ce trebuie pentru ET_REL) ----
struct Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Shdr {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};
struct Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};
struct Rela {
    uint64_t offset, info;
    int64_t addend;
};

const uint16_t ET_REL = 1;
const uint16_t EM_X86_64 = 62;
const uint32_t SHT_SYMTAB = 2, SHT_RELA = 4, SHT_NOBITS = 8, SHT_NULL = 0;
const uint64_t SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4;
const uint16_t SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2, SHN_LORESERVE = 0xff00;
const uint8_t STB_WEAK = 2;
const uint8_t STT_FUNC = 2, STT_OBJECT = 1;
const uint32_t R_NONE = 0, R_64 = 1, R_PC32 = 2, R_PLT32 = 4, R_GOTPCREL = 9, R_GOTPCRELX = 41, R_REX_GOTPCRELX = 42;

const int MAX_SECTIONS = 96;
const uint64_t MODULE_VA_BASE = 0xFFFFFFE000000000UL;   // pml4[511], pdpt[384]: intrarile pdpt noi se vad in toate spatiile de adrese
const uint64_t MODULE_VA_END = MODULE_VA_BASE + (64UL << 20);

// ---- simboluri exportate ----
struct Export {
    const char* name;
    uint64_t addr;
};

#define EXPORT(sym) {#sym, (uint64_t)(void*)&sym}
const Export exports[] = {
    EXPORT(kprintf), EXPORT(kputc), EXPORT(kmalloc), EXPORT(kzalloc), EXPORT(kfree),
    EXPORT(memcpy), EXPORT(memset), EXPORT(memmove), EXPORT(memcmp),
};

struct Module {
    bool used;
    char name[32];
    uint64_t va;                    // inceputul imaginii mapate
    uint64_t phys;                  // prima pagina (contigue)
    uint64_t pages;
    void (*exit_fn)();
};

Module modules[KMOD_MAX];
Mutex kmod_mutex;
uint64_t next_va = MODULE_VA_BASE;

bool streq(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

bool valid_name(const char* n){
    size_t len = 0;
    for(; n[len]; len++){
        char c = n[len];
        if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') || len >= 31){
            return false;
        }
    }
    return len > 0;
}

uint64_t align_up(uint64_t v, uint64_t a){
    return a > 1 ? (v + a - 1) / a * a : v;
}

bool in_range(uint64_t off, uint64_t size, uint64_t total){
    return off <= total && size <= total - off;
}

struct Layout {
    uint64_t base[MAX_SECTIONS];        // offset din imagine; ~0 daca sectiunea nu se incarca
    int region[MAX_SECTIONS];           // 0 text, 1 rodata(+GOT), 2 date
};

int64_t load_image(const uint8_t* elf, uint64_t elf_len, int slot){
    if(elf_len < sizeof(Ehdr)){
        return -ENOEXEC;
    }
    const Ehdr* eh = (const Ehdr*)elf;
    if(memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != 2 || eh->ident[5] != 1 || eh->type != ET_REL ||
       eh->machine != EM_X86_64 || eh->shentsize != sizeof(Shdr) || eh->shnum == 0 || eh->shnum > MAX_SECTIONS ||
       !in_range(eh->shoff, (uint64_t)eh->shnum * sizeof(Shdr), elf_len)){
        return -ENOEXEC;
    }
    const Shdr* sh = (const Shdr*)(elf + eh->shoff);
    int n = eh->shnum;
    for(int i = 0; i < n; i++){
        if(sh[i].type != SHT_NOBITS && sh[i].type != SHT_NULL && !in_range(sh[i].offset, sh[i].size, elf_len)){
            return -ENOEXEC;
        }
        if(sh[i].addralign > 4096){
            return -ENOEXEC;
        }
    }
    // Tabela de simboluri
    int symtab = -1;
    for(int i = 0; i < n; i++){
        if(sh[i].type == SHT_SYMTAB){
            if(symtab >= 0){
                return -ENOEXEC;
            }
            symtab = i;
        }
    }
    if(symtab < 0 || sh[symtab].entsize != sizeof(Sym) || sh[symtab].link >= (uint32_t)n){
        return -ENOEXEC;
    }
    const Sym* syms = (const Sym*)(elf + sh[symtab].offset);
    uint64_t nsyms = sh[symtab].size / sizeof(Sym);
    const Shdr& strsh = sh[sh[symtab].link];
    const char* strtab = (const char*)(elf + strsh.offset);
    uint64_t strsz = strsh.size;

    // Asezare: text | rodata | GOT | date; stub-urile PLT la sfarsitul textului, GOT dupa rodata. Un slot de GOT / stub per relocare
    uint64_t nrela_total = 0;
    for(int i = 0; i < n; i++){
        if(sh[i].type == SHT_RELA){
            if(sh[i].entsize != sizeof(Rela) || sh[i].info >= (uint32_t)n || sh[i].link != (uint32_t)symtab){
                return -ENOEXEC;
            }
            nrela_total += sh[i].size / sizeof(Rela);
        }
    }
    if(nrela_total > 8192){
        return -E2BIG;
    }
    static Layout lay;                  // sub kmod_mutex
    uint64_t region_size[3] = {0, 0, 0};
    for(int i = 0; i < n; i++){
        lay.base[i] = ~0UL;
        if(!(sh[i].flags & SHF_ALLOC) || sh[i].size == 0){
            continue;
        }
        int r = (sh[i].flags & SHF_EXECINSTR) ? 0 : (sh[i].flags & SHF_WRITE) ? 2 : 1;
        lay.region[i] = r;
        lay.base[i] = align_up(region_size[r], sh[i].addralign ? sh[i].addralign : 1);
        region_size[r] = lay.base[i] + sh[i].size;
    }
    uint64_t stub_off = align_up(region_size[0], 16);
    region_size[0] = stub_off + nrela_total * 16;
    uint64_t got_off = align_up(region_size[1], 8);
    region_size[1] = got_off + nrela_total * 8;
    uint64_t reg_off[3];
    reg_off[0] = 0;
    reg_off[1] = align_up(reg_off[0] + region_size[0], PAGE_SIZE);
    reg_off[2] = align_up(reg_off[1] + region_size[1], PAGE_SIZE);
    uint64_t total = align_up(reg_off[2] + region_size[2], PAGE_SIZE);
    if(total == 0 || total > KMOD_MAX_IMAGE + (nrela_total * 24 + 3 * PAGE_SIZE)){
        return -E2BIG;
    }
    uint64_t pages = total / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contig(pages);
    if(!phys){
        return -ENOMEM;
    }
    uint8_t* mem = (uint8_t*)phys_to_virt(phys);
    memset(mem, 0, total);
    if(next_va + total + PAGE_SIZE > MODULE_VA_END){
        for(uint64_t p = 0; p < pages; p++){
            pmm_free_page(phys + p * PAGE_SIZE);
        }
        return -ENOMEM;
    }
    uint64_t va = next_va;
    next_va += total + PAGE_SIZE;       // pagina de garda (nemapata) intre module

    int64_t rc = 0;
    // Copiaza sectiunile PROGBITS
    uint64_t sec_off[MAX_SECTIONS];     // offset final din imagine
    for(int i = 0; i < n; i++){
        sec_off[i] = ~0UL;
        if(lay.base[i] == ~0UL){
            continue;
        }
        sec_off[i] = reg_off[lay.region[i]] + lay.base[i];
        if(sh[i].type != SHT_NOBITS){
            memcpy(mem + sec_off[i], elf + sh[i].offset, sh[i].size);
        }
    }
    // Relocari
    uint64_t stubs_used = 0, got_used = 0;
    for(int i = 0; i < n && rc == 0; i++){
        if(sh[i].type != SHT_RELA){
            continue;
        }
        uint32_t target = sh[i].info;
        if(sec_off[target] == ~0UL){
            continue;                   // relocari pentru sectiuni care nu se incarca (.eh_frame, .comment...)
        }
        const Rela* rel = (const Rela*)(elf + sh[i].offset);
        uint64_t cnt = sh[i].size / sizeof(Rela);
        for(uint64_t k = 0; k < cnt && rc == 0; k++){
            uint32_t type = (uint32_t)rel[k].info;
            uint64_t si = rel[k].info >> 32;
            if(rel[k].offset > sh[target].size || sh[target].size - rel[k].offset < (type == R_64 ? 8u : (type == R_NONE ? 0u : 4u))){
                rc = -ENOEXEC;
                break;
            }
            uint64_t S = 0;
            bool external = false;
            if(type != R_NONE){
                if(si == 0 || si >= nsyms){
                    rc = -ENOEXEC;
                    break;
                }
                const Sym& sy = syms[si];
                if(sy.shndx == SHN_UNDEF){
                    external = true;
                    if(sy.name >= strsz){
                        rc = -ENOEXEC;
                        break;
                    }
                    const char* nm = strtab + sy.name;
                    bool found = false;
                    for(const Export& e : exports){
                        const char* a = e.name;
                        const char* b = nm;
                        while(*a && *a == *b){
                            a++;
                            b++;
                        }
                        if(!*a && !*b){
                            S = e.addr;
                            found = true;
                            break;
                        }
                    }
                    if(!found && (sy.info >> 4) != STB_WEAK){
                        kprintf("kmod: simbol nerezolvat: %s\n", nm);
                        rc = -ENOENT;
                        break;
                    }
                } else if(sy.shndx == SHN_ABS){
                    S = sy.value;
                } else if(sy.shndx >= SHN_LORESERVE || sy.shndx >= n || sec_off[sy.shndx] == ~0UL){
                    rc = -ENOEXEC;      // COMMON (compileaza cu -fno-common) sau sectiune neincarcata
                    break;
                } else {
                    S = va + sec_off[sy.shndx] + sy.value;
                }
            }
            uint64_t place = sec_off[target] + rel[k].offset;          // in mem
            uint64_t P = va + place;                                   // adresa finala
            int64_t A = rel[k].addend;
            if(type == R_NONE){
                continue;
            } else if(type == R_64){
                uint64_t v = S + (uint64_t)A;
                memcpy(mem + place, &v, 8);
            } else if(type == R_PC32 || type == R_PLT32){
                uint64_t dest = S;
                if(external){
                    // kernelul e la >2 GiB de modul: sarituri printr-un stub "jmp *0(%rip); .quad adresa" in modul
                    if(type != R_PLT32 || !(sh[target].flags & SHF_EXECINSTR)){
                        rc = -ENOEXEC;
                        break;
                    }
                    uint64_t so = stub_off + stubs_used * 16;
                    stubs_used++;
                    mem[so] = 0xff;
                    mem[so + 1] = 0x25;
                    memset(mem + so + 2, 0, 4);
                    memcpy(mem + so + 6, &S, 8);
                    dest = va + so;
                }
                int64_t v = (int64_t)(dest + (uint64_t)A - P);
                if(v != (int32_t)v){
                    rc = -ENOEXEC;
                    break;
                }
                int32_t v32 = (int32_t)v;
                memcpy(mem + place, &v32, 4);
            } else if(type == R_GOTPCREL || type == R_GOTPCRELX || type == R_REX_GOTPCRELX){
                uint64_t slot = reg_off[1] + got_off + got_used * 8;
                got_used++;
                memcpy(mem + slot, &S, 8);
                int64_t v = (int64_t)(va + slot + (uint64_t)A - P);
                if(v != (int32_t)v){
                    rc = -ENOEXEC;
                    break;
                }
                int32_t v32 = (int32_t)v;
                memcpy(mem + place, &v32, 4);
            } else {
                kprintf("kmod: relocare nesuportata: tip %u\n", type);
                rc = -ENOEXEC;
            }
        }
    }
    // Simbolurile modulului
    uint64_t init_off = ~0UL, exit_off = ~0UL, name_off = ~0UL;
    for(uint64_t i = 1; rc == 0 && i < nsyms; i++){
        const Sym& sy = syms[i];
        if(sy.shndx == SHN_UNDEF || sy.shndx >= n || sec_off[sy.shndx] == ~0UL || sy.name >= strsz){
            continue;
        }
        const char* nm = strtab + sy.name;
        uint64_t off = sec_off[sy.shndx] + sy.value;
        if(streq(nm, "yokai_module_init") && (sy.info & 15) == STT_FUNC){
            init_off = off;
        } else if(streq(nm, "yokai_module_exit") && (sy.info & 15) == STT_FUNC){
            exit_off = off;
        } else if(streq(nm, "yokai_module_name") && (sy.info & 15) == STT_OBJECT){
            name_off = off;
        }
    }
    if(rc == 0 && (init_off == ~0UL || name_off == ~0UL || init_off >= reg_off[1] || (exit_off != ~0UL && exit_off >= reg_off[1]))){
        rc = -ENOEXEC;
    }
    char name[32];
    if(rc == 0){
        uint64_t k = 0;
        for(; k < 31 && name_off + k < total && mem[name_off + k]; k++){
            name[k] = (char)mem[name_off + k];
        }
        name[k] = 0;
        if(!valid_name(name) || (name_off + k < total && mem[name_off + k])){
            rc = -ENOEXEC;
        }
    }
    for(int i = 0; rc == 0 && i < KMOD_MAX; i++){
        if(modules[i].used && streq(modules[i].name, name)){
            rc = -EEXIST;
        }
    }
    // Mapare cu drepturile finale: text RX, rodata+GOT R (NX), date RW (NX)
    uint64_t mapped = 0;
    for(uint64_t p = 0; rc == 0 && p < pages; p++){
        uint64_t off = p * PAGE_SIZE;
        uint64_t flags = off < reg_off[1] ? 0 : off < reg_off[2] ? PTE_NX : (PTE_RW | PTE_NX);
        if(!vmm_map(kernel_as, va + off, phys + off, flags)){
            rc = -ENOMEM;
        } else {
            mapped++;
        }
    }
    if(rc == 0){
        int (*init)() = (int (*)())(va + init_off);
        int r = init();
        if(r != 0){
            rc = r < 0 ? r : -EINVAL;
        }
    }
    if(rc != 0){
        for(uint64_t p = 0; p < mapped; p++){
            vmm_unmap(kernel_as, va + p * PAGE_SIZE);
        }
        for(uint64_t p = 0; p < pages; p++){
            pmm_free_page(phys + p * PAGE_SIZE);
        }
        return rc;
    }
    Module& m = modules[slot];
    m.used = true;
    memcpy(m.name, name, sizeof(m.name));
    m.va = va;
    m.phys = phys;
    m.pages = pages;
    m.exit_fn = exit_off == ~0UL ? nullptr : (void (*)())(va + exit_off);
    return 0;
}

}

int kmod_load(const void* blob, uint64_t len){
    uint64_t elf_len;
    int rc = sign_check_module(blob, len, elf_len);
    if(rc != 0){
        return rc;
    }
    if(elf_len > KMOD_MAX_IMAGE){
        return -E2BIG;
    }
    MutexGuard g(kmod_mutex);
    int slot = -1;
    for(int i = 0; i < KMOD_MAX; i++){
        if(!modules[i].used){
            slot = i;
            break;
        }
    }
    if(slot < 0){
        return -ENOMEM;
    }
    return (int)load_image((const uint8_t*)blob, elf_len, slot);
}

int kmod_unload(const char* name){
    MutexGuard g(kmod_mutex);
    for(int i = 0; i < KMOD_MAX; i++){
        Module& m = modules[i];
        if(!m.used || !streq(m.name, name)){
            continue;
        }
        if(m.exit_fn){
            m.exit_fn();
        }
        for(uint64_t p = 0; p < m.pages; p++){
            vmm_unmap(kernel_as, m.va + p * PAGE_SIZE);
        }
        for(uint64_t p = 0; p < m.pages; p++){
            pmm_free_page(m.phys + p * PAGE_SIZE);
        }
        m.used = false;
        return 0;
    }
    return -ENOENT;
}

bool kmod_loaded(const char* name){
    MutexGuard g(kmod_mutex);
    for(int i = 0; i < KMOD_MAX; i++){
        if(modules[i].used && streq(modules[i].name, name)){
            return true;
        }
    }
    return false;
}

int kmod_count(){
    MutexGuard g(kmod_mutex);
    int c = 0;
    for(int i = 0; i < KMOD_MAX; i++){
        c += modules[i].used ? 1 : 0;
    }
    return c;
}

int64_t sys_init_module(uint64_t image, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t){
    if(!capable(CAP_SYS_ADMIN)){
        return -EPERM;
    }
    if(len == 0 || len > KMOD_MAX_IMAGE + 4096){
        return -E2BIG;
    }
    if(!user_access_ok(image, len, false)){
        return -EFAULT;
    }
    uint8_t* buf = (uint8_t*)kmalloc(len);
    if(!buf){
        return -ENOMEM;
    }
    int64_t rc = copy_from_user(buf, image, len) ? kmod_load(buf, len) : -EFAULT;
    kfree(buf);
    return rc;
}

int64_t sys_delete_module(uint64_t name, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    if(!capable(CAP_SYS_ADMIN)){
        return -EPERM;
    }
    char n[32];
    int64_t len = copy_string_from_user(n, name, sizeof(n));
    if(len < 0){
        return len;
    }
    return kmod_unload(n);
}
