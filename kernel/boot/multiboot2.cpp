#include "boot/multiboot2.h"
#include "lib/kprintf.h"

BootInfo boot_info;

namespace {

enum TagType : uint32_t {
    TAG_END = 0,
    TAG_CMDLINE = 1,
    TAG_MODULE = 3,
    TAG_MMAP = 6,
    TAG_FRAMEBUFFER = 8,
    TAG_ACPI_OLD = 14,
    TAG_ACPI_NEW = 15,
};

struct Tag {
    uint32_t type;
    uint32_t size;
};

struct TagModule {
    Tag tag;
    uint32_t mod_start;
    uint32_t mod_end;
    char name[];
};

struct TagMmap {
    Tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct MmapEntry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct __attribute__((packed)) TagFramebuffer {
    Tag tag;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;
};

const char* const empty = "";

bool token_starts_with(const char* tok, const char* key, bool exact){
    while(*key){
        if(*tok++ != *key++){
            return false;
        }
    }
    return !exact || *tok == ' ' || *tok == '\0';
}

}

bool multiboot2_parse(uint32_t magic, uint64_t mbi_phys){
    boot_info.cmdline = empty;
    if(magic != MB2_BOOTLOADER_MAGIC){
        kprintf("multiboot2: magic gresit %x\n", magic);
        return false;
    }

    const uint8_t* mbi = (const uint8_t*)(KERNEL_VMA + mbi_phys);
    uint32_t total = *(const uint32_t*)mbi;

    for(uint32_t off = 8; off + sizeof(Tag) <= total; ){
        const Tag* tag = (const Tag*)(mbi + off);
        if(tag->type == TAG_END){
            break;
        }

        switch(tag->type){
            case TAG_CMDLINE:
                boot_info.cmdline = (const char*)(tag + 1);
                break;
            case TAG_MODULE: {
                const TagModule* m = (const TagModule*)tag;
                if(boot_info.module_count < 16){
                    BootModule& bm = boot_info.modules[boot_info.module_count++];
                    bm.start = m->mod_start;
                    bm.end = m->mod_end;
                    bm.name = m->name;
                }
                break;
            }
            case TAG_MMAP: {
                const TagMmap* t = (const TagMmap*)tag;
                for(uint32_t e = sizeof(TagMmap); e + sizeof(MmapEntry) <= t->tag.size; e += t->entry_size){
                    const MmapEntry* me = (const MmapEntry*)((const uint8_t*)t + e);
                    if(boot_info.mmap_count < 64){
                        MemRegion& r = boot_info.mmap[boot_info.mmap_count++];
                        r.base = me->base;
                        r.length = me->length;
                        r.type = me->type;
                    }
                }
                break;
            }
            case TAG_FRAMEBUFFER: {
                const TagFramebuffer* f = (const TagFramebuffer*)tag;
                boot_info.fb.addr = f->addr;
                boot_info.fb.pitch = f->pitch;
                boot_info.fb.width = f->width;
                boot_info.fb.height = f->height;
                boot_info.fb.bpp = f->bpp;
                boot_info.fb.type = f->type;
                boot_info.has_fb = true;
                break;
            }
            case TAG_ACPI_OLD:
            case TAG_ACPI_NEW:
                // Preferam RSDP-ul ACPI 2.0 daca exista ambele
                if(!boot_info.rsdp || tag->type == TAG_ACPI_NEW){
                    boot_info.rsdp = tag + 1;
                    boot_info.rsdp_version = tag->type == TAG_ACPI_NEW ? 2 : 1;
                }
                break;
        }

        off += (tag->size + 7) & ~7u;
    }
    return true;
}

uint64_t multiboot2_usable_ram(){
    uint64_t total = 0;
    for(uint32_t i = 0; i < boot_info.mmap_count; i++){
        if(boot_info.mmap[i].type == MB2_MMAP_AVAILABLE){
            total += boot_info.mmap[i].length;
        }
    }
    return total;
}

void multiboot2_print(){
    kprintf("cmdline: \"%s\"\n", boot_info.cmdline);
    kprintf("mmap (%u regiuni):\n", boot_info.mmap_count);
    for(uint32_t i = 0; i < boot_info.mmap_count; i++){
        const MemRegion& r = boot_info.mmap[i];
        kprintf("  %016lx - %016lx  tip %u\n", r.base, r.base + r.length, r.type);
    }
    for(uint32_t i = 0; i < boot_info.module_count; i++){
        const BootModule& m = boot_info.modules[i];
        kprintf("modul %s: %lx - %lx\n", m.name, m.start, m.end);
    }
    if(boot_info.has_fb){
        kprintf("framebuffer: %ux%u %u bpp tip %u la %lx\n", boot_info.fb.width, boot_info.fb.height,
                boot_info.fb.bpp, boot_info.fb.type, boot_info.fb.addr);
    }
    kprintf("RSDP: %s\n", boot_info.rsdp ? (boot_info.rsdp_version == 2 ? "ACPI 2.0" : "ACPI 1.0") : "lipseste");
    kprintf("RAM utilizabil: %lu MiB\n", multiboot2_usable_ram() >> 20);
}

bool cmdline_has(const char* key){
    for(const char* p = boot_info.cmdline; *p; ){
        if(token_starts_with(p, key, true)){
            return true;
        }
        if(token_starts_with(p, key, false) && p[0] && key[0]){
            int n = 0;
            while(key[n]){
                n++;
            }
            if(p[n] == '='){
                return true;
            }
        }
        while(*p && *p != ' '){
            p++;
        }
        while(*p == ' '){
            p++;
        }
    }
    return false;
}

const char* cmdline_get(const char* key_eq){
    for(const char* p = boot_info.cmdline; *p; ){
        if(token_starts_with(p, key_eq, false)){
            while(*key_eq){
                key_eq++;
                p++;
            }
            return p;
        }
        while(*p && *p != ' '){
            p++;
        }
        while(*p == ' '){
            p++;
        }
    }
    return nullptr;
}
