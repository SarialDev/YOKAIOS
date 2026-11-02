/* KASLR: ruleaza din boot.S in long mode, inainte de saltul in higher-half, la adresele FIZICE (identity map 0-1 GiB).
 * Se compileaza -fpie (doar adresare relativa la RIP) si nu cheama nimic din kernel: codul kernelului inca nu e relocat.
 * Alege un offset aleator aliniat la 2 MiB (RDRAND; fallback TSC + RTC), aplica tabelul de relocari generat de
 * tools/kaslr_relocs.py si mapeaza imaginea la KERNEL_VMA + offset in boot_pd_k. Cmdline "nokaslr" = offset 0. */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

#define HIDDEN __attribute__((visibility("hidden")))
#define PAGE_2M (1UL << 21)
#define KERNEL_WINDOW (1UL << 30)       /* boot_pd_k acopera KERNEL_VMA + [0, 1 GiB) */
#define RELOC_MAGIC 0x314C524BU         /* "KRL1" */

struct RelocHeader {
    uint32_t magic;
    uint32_t n64;                       /* R_X86_64_64: valoare pe 64 biti */
    uint32_t n32;                       /* R_X86_64_32S: valoare pe 32 biti extinsa cu semn */
    uint32_t reserved;
    /* urmeaza n64 + n32 adrese fizice (adresa de link - KERNEL_VMA) ale locurilor de corectat */
};

extern char _kernel_end[] HIDDEN;
extern char __kaslr_relocs[] HIDDEN;
extern char __kaslr_relocs_end[] HIDDEN;
extern uint64_t boot_pd_k[512] HIDDEN;
/* in .data: .bss e zero oricum (GRUB), dar asa e clar ca valoarea vine din stub */
uint64_t kaslr_offset __attribute__((section(".data"))) = 0;
uint32_t kaslr_flags __attribute__((section(".data"))) = 0;   /* bit 0: RDRAND, bit 1: nokaslr */

static inline uint64_t rdtsc(void){
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static inline uint8_t cmos(uint8_t reg){
    uint8_t v;
    __asm__ volatile("outb %0, $0x70" : : "a"(reg));
    __asm__ volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

static uint64_t mix(uint64_t x){
    x += 0x9E3779B97F4A7C15UL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9UL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBUL;
    return x ^ (x >> 31);
}

static uint64_t entropy(void){
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if(c & (1u << 30)){
        for(int i = 0; i < 10; i++){
            uint64_t v;
            uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if(ok){
                kaslr_flags |= 1;
                return v;
            }
        }
    }
    /* Fallback: TSC (variaza de la un boot la altul) + ceasul RTC (si sub -icount, unde TSC e determinist) */
    uint64_t t = rdtsc();
    uint64_t rtc = (uint64_t)cmos(0) | (uint64_t)cmos(2) << 8 | (uint64_t)cmos(4) << 16 | (uint64_t)cmos(7) << 24;
    return mix(t ^ mix(rtc));
}

/* "nokaslr" ca token separat prin spatii in cmdline (tag-ul Multiboot2 de tip 1) */
static int has_nokaslr(uint32_t mbi_phys){
    const uint8_t* mbi = (const uint8_t*)(uint64_t)mbi_phys;
    uint32_t total = *(const uint32_t*)mbi;
    for(uint32_t off = 8; off + 8 <= total; ){
        uint32_t type = *(const uint32_t*)(mbi + off);
        uint32_t size = *(const uint32_t*)(mbi + off + 4);
        if(type == 0){
            break;
        }
        if(type == 1){
            const char* s = (const char*)(mbi + off + 8);
            static const char key[] = "nokaslr";
            for(const char* p = s; *p; p++){
                if(p != s && p[-1] != ' '){
                    continue;
                }
                int i = 0;
                while(key[i] && p[i] == key[i]){
                    i++;
                }
                if(!key[i] && (p[i] == 0 || p[i] == ' ')){
                    return 1;
                }
            }
        }
        off += (size + 7) & ~7u;
    }
    return 0;
}

/* Intoarce offset-ul ales (multiplu de 2 MiB); imaginea e deja corectata si mapata cand se intoarce */
uint64_t kaslr_setup(uint32_t mbi_phys){
    uint64_t end = ((uint64_t)_kernel_end + PAGE_2M - 1) & ~(PAGE_2M - 1);   /* adresa fizica (rulam identity) */
    uint64_t slots = (KERNEL_WINDOW - end) / PAGE_2M + 1;                   /* offset-uri posibile: 0 .. slots-1 */
    uint64_t off = 0;
    if(has_nokaslr(mbi_phys)){
        kaslr_flags |= 2;
    } else {
        off = (entropy() % slots) * PAGE_2M;
    }

    const struct RelocHeader* h = (const struct RelocHeader*)__kaslr_relocs;
    if(off && (uint64_t)(__kaslr_relocs_end - __kaslr_relocs) >= sizeof(*h) && h->magic == RELOC_MAGIC){
        const uint32_t* r = (const uint32_t*)(h + 1);
        for(uint32_t i = 0; i < h->n64; i++){
            *(uint64_t*)(uint64_t)r[i] += off;
        }
        r += h->n64;
        for(uint32_t i = 0; i < h->n32; i++){
            *(uint32_t*)(uint64_t)r[i] += (uint32_t)off;
        }
    } else {
        off = 0;                        /* fara tabel nu se poate muta */
    }
    for(uint64_t i = 0; i < end / PAGE_2M; i++){
        boot_pd_k[off / PAGE_2M + i] = (i * PAGE_2M) | 0x83;   /* P | RW | PS */
    }
    kaslr_offset = off;
    return off;
}
