#include "core/panic.h"
#include "lib/kprintf.h"
#include "drivers/vga.h"
#include "boot/multiboot2.h"
#include "io.h"

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

uint64_t read_cr0(){ uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
uint64_t read_cr2(){ uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
uint64_t read_cr3(){ uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
uint64_t read_cr4(){ uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }

[[noreturn]] void halt(){
    for(;;){
        __asm__ volatile("cli; hlt");
    }
}

// Intra in panic o singura data; o a doua panic (din panic) doar opreste CPU-ul
void enter_panic(){
    __asm__ volatile("cli");
    if(in_panic){
        halt();
    }
    in_panic = true;
    vga_set_color(COLOR_PANIC);
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

void panic_frame(InterruptFrame* f, const char* msg){
    enter_panic();
    kprintf("\nPANIC: %s\n", msg);
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
