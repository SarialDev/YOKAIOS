#pragma once
#include "types.h"

const uint32_t MB2_BOOTLOADER_MAGIC = 0x36D76289;
const uint32_t MB2_MMAP_AVAILABLE = 1;

struct MemRegion {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

struct BootFramebuffer {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;   // 0 indexat, 1 RGB, 2 text EGA
    // Doar pentru type 1: pozitia (bit) si marimea fiecarui canal
    uint8_t red_pos, red_size;
    uint8_t green_pos, green_size;
    uint8_t blue_pos, blue_size;
};

struct BootModule {
    uint64_t start;
    uint64_t end;
    const char* name;
};

struct BootInfo {
    MemRegion mmap[64];
    uint32_t mmap_count;
    BootFramebuffer fb;
    bool has_fb;
    const char* cmdline;
    BootModule modules[16];
    uint32_t module_count;
    const void* rsdp;       // copia RSDP din tag-ul 14/15
    uint8_t rsdp_version;   // 1 = ACPI 1.0, 2 = ACPI 2.0+
    uint64_t mbi_phys;      // structura Multiboot2 (pmm o rezerva)
    uint32_t mbi_size;
};

extern BootInfo boot_info;

// Modulul Multiboot2 cu numele (cmdline-ul din grub.cfg) exact "name", nullptr daca lipseste
const BootModule* boot_find_module(const char* name);

bool multiboot2_parse(uint32_t magic, uint64_t mbi_phys);
uint64_t multiboot2_usable_ram();
void multiboot2_print();

// "key" exact sau "key=..." ca token separat prin spatii
bool cmdline_has(const char* key);
// cmdline_get("key=") -> pointer la valoare (se termina la spatiu sau \0), nullptr daca lipseste
const char* cmdline_get(const char* key_eq);
