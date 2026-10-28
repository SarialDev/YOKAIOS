#include "core/panic.h"
#include "lib/kprintf.h"
#include "hal/console.h"
#include "boot/multiboot2.h"
#include "io.h"
#include "arch/x86_64/smp.h"

namespace {

const char* const EXCEPTION_NAMES[32] = {
    "#DE Divide Error", "#DB Debug", "NMI Interrupt", "#BP Breakpoint",
    "#OF Overflow", "#BR BOUND Range Exceeded", "#UD Invalid Opcode", "#NM Device Not Available",
    "#DF Double Fault", "Coprocessor Segment Overrun", "#TS Invalid TSS", "#NP Segment Not Present",
    "#SS Stack-Segment Fault", "#GP General Protection", "#PF Page Fault", "Reserved 15",
    "#MF x87 FPU Error", "#AC Alignment Check", "#MC Machine Check", "#XM SIMD Exception",
    "#VE Virtualization Exception", "#CP Control Protection", "Reserved 22", "Reserved 23",
    "Reserved 24", "Reserved 25", "Reserved 26", "Reserved 27",
    "#HV Hypervisor Injection", "#VC VMM Communication", "#SX Security Exception", "Reserved 31",
};

const uint8_t COLOR_PANIC = 0x0C;      // rosu deschis pe negru
const uint8_t EXIT_PANIC = 0x11;       // isa-debug-exit: QEMU iese cu codul 35
const int MAX_FRAMES = 16;

volatile bool in_panic = false;
bool guard_from_rdrand = false;

const int RDRAND_RETRIES = 10;
const uint32_t CPUID_RDRAND = 1u << 30;     // CPUID.1:ECX

// Fiecare apel poate esua (CF=0) daca generatorul e ocupat; se reincearca de cateva ori
__attribute__((no_stack_protector)) bool rdrand64(uint64_t& out){
    for(int i = 0; i < RDRAND_RETRIES; i++){
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

uint64_t read_cr0(){ uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
uint64_t read_cr2(){ uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
uint64_t read_cr3(){ uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
uint64_t read_cr4(){ uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }

[[noreturn]] void halt(){
    for(;;){
        __asm__ volatile("cli; hlt");
    }
}

// Intra in panic o singura data; o a doua panic (din panic sau de pe alt CPU) doar opreste CPU-ul
void enter_panic(){
    __asm__ volatile("cli");
    if(__atomic_exchange_n(&in_panic, true, __ATOMIC_SEQ_CST)){
        halt();
    }
    // Celelalte CPU-uri se opresc (NMI), apoi consola e a acestui CPU chiar daca altul o tinea
    smp_stop_others();
    for(volatile int i = 0; i < 1000000; i++){
    }
    console_break_lock();
    hal_console_set_color(COLOR_PANIC);
}

void print_control_regs(){
    kprintf("CR0=%016lx CR2=%016lx\nCR3=%016lx CR4=%016lx\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());
}

// Lantul RBP: [rbp] = rbp-ul apelantului, [rbp+8] = adresa de retur
void print_stack_trace(uint64_t rbp){
    for(int i = 0; i < MAX_FRAMES; i++){
        if(rbp < KERNEL_VMA || (rbp & 7)){
            break;
        }
        const uint64_t* frame = (const uint64_t*)rbp;
        uint64_t ret = frame[1];
        if(ret < KERNEL_VMA){
            break;
        }
        kprintf("  #%d %016lx\n", i, ret);
        if(frame[0] <= rbp){
            break;
        }
        rbp = frame[0];
    }
}

[[noreturn]] void finish(){
    if(cmdline_has("test")){
        outb(0xF4, EXIT_PANIC);
    }
    halt();
}

}

const char* exception_name(uint64_t vec){
    return vec < 32 ? EXCEPTION_NAMES[vec] : "Unknown";
}

void panic(const char* fmt, ...){
    enter_panic();
    kprintf("\nPANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    print_control_regs();
    kprintf("Stack trace:\n");
    print_stack_trace((uint64_t)__builtin_frame_address(0));
    finish();
}

void panic_frame(InterruptFrame* f, const char* fmt, ...){
    enter_panic();
    kprintf("\nPANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    kprintf("vector=%lu (%s) error=%lx\n", f->vector, exception_name(f->vector), f->error);
    kprintf("RIP=%016lx CS=%04lx RFLAGS=%016lx\nRSP=%016lx SS=%04lx\n",
            f->rip, f->cs, f->rflags, f->rsp, f->ss);
    print_control_regs();
    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("RDX=%016lx RSI=%016lx RDI=%016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("RBP=%016lx R8 =%016lx R9 =%016lx\n", f->rbp, f->r8, f->r9);
    kprintf("R10=%016lx R11=%016lx R12=%016lx\n", f->r10, f->r11, f->r12);
    kprintf("R13=%016lx R14=%016lx R15=%016lx\n", f->r13, f->r14, f->r15);
    kprintf("Stack trace:\n  -> %016lx\n", f->rip);
    print_stack_trace(f->rbp);
    finish();
}

// Valoare fixa pana la stack_guard_init; byte-ul de jos 0 opreste copierile de stringuri
uint64_t __stack_chk_guard = 0x595A4B4F49535300UL;

// Ruleaza inaintea oricarei functii protejate, deci nu are voie sa aiba ea insasi canary
extern "C" __attribute__((no_stack_protector)) void stack_guard_init(){
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uint64_t guard = 0;
    guard_from_rdrand = (c & CPUID_RDRAND) && rdrand64(guard);
    if(!guard_from_rdrand){
        // Fallback: TSC amestecat (splitmix64), mai bun decat o constanta
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        guard = ((uint64_t)hi << 32 | lo) + 0x9E3779B97F4A7C15UL;
        guard = (guard ^ (guard >> 30)) * 0xBF58476D1CE4E5B9UL;
        guard = (guard ^ (guard >> 27)) * 0x94D049BB133111EBUL;
        guard ^= guard >> 31;
    }
    __stack_chk_guard = guard & ~0xFFUL;
}

extern "C" void __stack_chk_fail(){
    panic("stack smashing detected (canary suprascris)");
}

const char* stack_guard_source(){
    return guard_from_rdrand ? "RDRAND" : "TSC";
}
