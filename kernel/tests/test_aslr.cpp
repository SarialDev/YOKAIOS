#include "tests/test_aslr.h"
#include "core/kaslr.h"
#include "core/panic.h"
#include "exec/elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "lib/kprintf.h"
#include "lib/string.h"

extern "C" char _kernel_start[];
extern "C" char _kernel_end[];
extern "C" void kernel_main(uint32_t magic, uint64_t mbi_phys);

namespace {

const char* const envp[] = {"PATH=/bin:/sbin", nullptr};
int fails = 0;

void check(bool ok, const char* what){
    if(!ok){
        fails++;
        kprintf("test_aslr: FAIL %s\n", what);
    }
}

#define CHECK(c) check((c), #c)

// Initializat la link (R_X86_64_64 in .data), corectat de kaslr_setup
void (*volatile reloc_ptr)(uint32_t, uint64_t) = kernel_main;

void kaslr_checks(){
    const uint64_t base = KERNEL_VMA + kaslr_offset;
    CHECK(kaslr_offset % (2UL << 20) == 0);
    CHECK(kaslr_offset + kernel_phys(_kernel_end) <= (1UL << 30));
    CHECK((uint64_t)_kernel_start == base + (1UL << 20));
    CHECK(kernel_phys(_kernel_start) == (1UL << 20));
    // aceeasi adresa pe trei cai: relativ la RIP, imediat 32S (mcmodel=kernel) si pointer din .data
    uint64_t rip_rel, imm32;
    __asm__("leaq kernel_main(%%rip), %0" : "=r"(rip_rel));
    __asm__("movq $kernel_main, %0" : "=r"(imm32));
    CHECK(rip_rel == imm32);
    CHECK(rip_rel == (uint64_t)reloc_ptr);
    CHECK(rip_rel >= base && rip_rel < (uint64_t)_kernel_end);
    // tabelele de pagini: virtual -> fizic prin offset
    CHECK(vmm_translate(kernel_as, rip_rel) == kernel_phys((const void*)rip_rel));
    CHECK(vmm_translate(kernel_as, (uint64_t)&kaslr_offset) == kernel_phys(&kaslr_offset));
    // fereastra veche KERNEL_VMA + fizic nu mai exista (cu offset != 0 adresa KERNEL_VMA nu e mapata)
    if(kaslr_offset >= (4UL << 20)){
        CHECK(vmm_translate(kernel_as, KERNEL_VMA) == 0);
    }
}

void layout_checks(){
    UserLayout a = aslr_layout(), b = aslr_layout();
    const UserLayout* both[] = {&a, &b};
    for(const UserLayout* l : both){
        CHECK(l->pie_base >= ET_DYN_BASE && l->pie_base < ET_DYN_BASE + ASLR_PIE_PAGES * PAGE_SIZE);
        CHECK(l->mmap_base >= MMAP_BASE && l->mmap_base < MMAP_BASE + ASLR_MMAP_PAGES * PAGE_SIZE);
        CHECK(l->stack_top <= USER_STACK_TOP && l->stack_top > USER_STACK_TOP - ASLR_STACK_PAGES * PAGE_SIZE);
        CHECK(l->brk_gap < ASLR_BRK_PAGES * PAGE_SIZE);
        CHECK(!(l->pie_base % PAGE_SIZE) && !(l->mmap_base % PAGE_SIZE) && !(l->stack_top % PAGE_SIZE) &&
              !(l->brk_gap % PAGE_SIZE) && l->sp_offset < PAGE_SIZE && !(l->sp_offset % 16));
        CHECK(l->stack_top - USER_STACK_MAX > MMAP_END);
    }
    if(aslr_enabled()){
        CHECK(a.stack_top != b.stack_top || a.pie_base != b.pie_base);
        CHECK(a.mmap_base != b.mmap_base || a.pie_base != b.pie_base);
    } else {
        CHECK(a.stack_top == USER_STACK_TOP && a.pie_base == ET_DYN_BASE && a.mmap_base == MMAP_BASE && !a.brk_gap);
    }
}

// ELF-uri refuzate: ET_DYN cu PT_INTERP (ar cere dynamic loader)
void elf_checks(){
    static uint8_t buf[64 + 2 * 56];
    memset(buf, 0, sizeof(buf));
    const uint8_t ident[8] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
    memcpy(buf, ident, 8);
    auto u16 = [](uint8_t* p, uint16_t v){ memcpy(p, &v, 2); };
    auto u32 = [](uint8_t* p, uint32_t v){ memcpy(p, &v, 4); };
    auto u64 = [](uint8_t* p, uint64_t v){ memcpy(p, &v, 8); };
    u16(buf + 16, 3);                   // ET_DYN
    u16(buf + 18, 62);
    u32(buf + 20, 1);
    u64(buf + 24, 0x1000);              // entry
    u64(buf + 32, 64);                  // phoff
    u16(buf + 54, 56);
    u16(buf + 56, 2);
    uint8_t* ph = buf + 64;
    u32(ph, 3);                         // PT_INTERP
    u32(ph + 56, 1);                    // PT_LOAD R+X la 0
    u32(ph + 56 + 4, 5);
    u64(ph + 56 + 32, sizeof(buf));
    u64(ph + 56 + 40, 0x2000);
    AddressSpace as = as_create();
    CHECK(as.pml4_phys);
    ElfImage img;
    CHECK(elf_load(as, buf, sizeof(buf), img, ET_DYN_BASE) == -EINVAL);
    u32(ph, 6);                         // PT_PHDR in loc de PT_INTERP: se incarca la baza ceruta
    CHECK(elf_load(as, buf, sizeof(buf), img, ET_DYN_BASE + 0x5000) == 0);
    CHECK(img.bias == ET_DYN_BASE + 0x5000 && img.entry == ET_DYN_BASE + 0x6000 && img.nrelocs == 0);
    CHECK(vmm_translate(as, ET_DYN_BASE + 0x5000) != 0 && vmm_translate(as, 0x1000) == 0);
    CHECK(elf_load(as, buf, sizeof(buf), img, ET_DYN_BASE + 0x123) == -EINVAL);     // baza nealiniata
    as_destroy(as);
}

int run(const char* path, const char* const* argv){
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn(path, argv, envp);
    if(!pid){
        panic("test_aslr: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_aslr: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_aslr_run(){
    __asm__ volatile("sti");
    kprintf("[aslr] kernel_main=%p offset=%lx (%s) user=%s\n", (void*)&kernel_main, kaslr_offset, kaslr_source(),
            aslr_enabled() ? "on" : "off");
    kaslr_checks();
    layout_checks();
    elf_checks();
    if(fails){
        panic("test_aslr: %d verificari esuate", fails);
    }
    static const char* const argv[] = {"aslrtest", nullptr};
    uint64_t free_before = pmm_free_count();
    int code = run("/bin/aslrtest", argv);
    if(code != 0){
        panic("test_aslr: aslrtest a iesit cu %d (vezi liniile \"aslrtest: FAIL\")", code);
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_aslr: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_aslr (KASLR offset %lx, stiva/brk/mmap/PIE aleatoare)\n", kaslr_offset);
}
