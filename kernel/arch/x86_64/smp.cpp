#include "arch/x86_64/smp.h"
#include "arch/x86_64/pat.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "hal/irqchip.h"
#include "acpi/acpi.h"
#include "mm/vmm.h"
#include "mm/buddy.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "syscall/syscall.h"
#include "time/clock.h"
#include "sync/spinlock.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/string.h"

PerCpu cpus[MAX_CPUS];

extern "C" uint8_t stack_top[];
extern "C" uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern "C" uint8_t tr_cr3[], tr_efer[], tr_stack[], tr_entry[], tr_cpu[];

namespace {

const uint64_t TRAMPOLINE_PHYS = 0x8000;
const uint32_t MSR_EFER = 0xC0000080;
const uint32_t MSR_GS_BASE = 0xC0000101;
const uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;
const uint64_t EFER_LMA = 1UL << 10;
const int IST_ORDER = 2;                        // 16 KiB
static_assert(PAGE_SIZE << IST_ORDER == PERCPU_IST_SIZE, "IST = 2^IST_ORDER pagini");

const uint64_t INIT_DELAY_NS = 10000000;        // 10 ms dupa INIT
const uint64_t SIPI_DELAY_NS = 200000;          // 200 us intre cele doua SIPI
const uint64_t ONLINE_TIMEOUT_NS = 100000000;   // 100 ms pana raspunde AP-ul
const uint64_t TLB_TIMEOUT_NS = 100000000;
const uint64_t KERNEL_HALF_START = 0xFFFF800000000000UL;
static_assert(MAX_CPUS <= 32, "masca de CPU-uri pe 32 de biti");

uint8_t bsp_ist1[PERCPU_IST_SIZE] __attribute__((aligned(16)));
volatile uint32_t online_count = 1;
uint64_t bsp_cr0 = 0;
uint64_t bsp_cr4 = 0;

Spinlock tlb_lock("tlb");
volatile uint64_t tlb_virt = 0;
volatile bool stopping = false;

uint64_t rdmsr(uint32_t msr){
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void wrmsr(uint32_t msr, uint64_t v){
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void delay_ns(uint64_t ns){
    uint64_t end = monotonic_ns() + ns;
    while(monotonic_ns() < end){
        cpu_relax();
    }
}

void flush_pending(PerCpu* c){
    if(__atomic_load_n(&c->tlb_pending, __ATOMIC_ACQUIRE)){
        invlpg(tlb_virt);
        __atomic_store_n(&c->tlb_pending, false, __ATOMIC_RELEASE);
    }
}

void tlb_ipi(InterruptFrame*){
    flush_pending(this_cpu());
}

// need_resched e pus de cel care trimite; sched_irq_exit face comutarea
void resched_ipi(InterruptFrame*){
    this_cpu()->ipis++;
}

[[noreturn]] void halt_forever(){
    for(;;){
        __asm__ volatile("cli; hlt");
    }
}

void nmi(InterruptFrame* f){
    if(stopping){
        halt_forever();
    }
    panic_frame(f, "NMI");
}

void patch(uint8_t* copy, const uint8_t* field, const void* value, size_t size){
    memcpy(copy + (field - ap_trampoline_start), value, size);
}

// INIT, 10 ms, SIPI, 200 us, SIPI daca nu a pornit; apoi asteapta online cu timeout
bool start_ap(PerCpu& c){
    hal_irqchip()->send_init(c.lapic_id);
    delay_ns(INIT_DELAY_NS);
    for(int attempt = 0; attempt < 2 && !c.online; attempt++){
        hal_irqchip()->send_startup(c.lapic_id, (uint8_t)(TRAMPOLINE_PHYS >> 12));
        delay_ns(SIPI_DELAY_NS);
    }
    uint64_t deadline = monotonic_ns() + ONLINE_TIMEOUT_NS;
    while(!__atomic_load_n(&c.online, __ATOMIC_ACQUIRE)){
        if(monotonic_ns() > deadline){
            return false;
        }
        cpu_relax();
    }
    return true;
}

}

// Prima functie C pe un AP, pe stiva task-ului lui idle
extern "C" [[noreturn]] void ap_main(uint32_t id){
    PerCpu& c = cpus[id];
    __asm__ volatile("mov %0, %%cr0" : : "r"(bsp_cr0) : "memory");
    __asm__ volatile("mov %0, %%cr4" : : "r"(bsp_cr4) : "memory");
    percpu_init(id, c.lapic_id);
    gdt_init_cpu(c);
    pat_init_cpu();
    idt_load();
    syscall_init_cpu();
    hal_irqchip()->init_ap();
    sched_init_cpu(c.idle);
    clock_init_ap();
    __atomic_fetch_add(&online_count, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c.online, true, __ATOMIC_RELEASE);
    sched_idle();
}

void percpu_init(uint32_t id, uint32_t lapic_id){
    PerCpu& c = cpus[id];
    c.self = &c;
    c.id = id;
    c.lapic_id = lapic_id;
    if(id == 0){
        c.ist1 = bsp_ist1;
        c.kstack_top = (uint64_t)stack_top;
        c.online = true;
    }
    // In kernel GS arata spre PerCpu; baza user (0) sta in KERNEL_GS_BASE pana la swapgs
    wrmsr(MSR_GS_BASE, (uint64_t)&c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

uint32_t smp_cpu_count(){
    return online_count;
}

void smp_init(){
    PerCpu& bsp = cpus[0];
    bsp.lapic_id = hal_irqchip()->cpu_id();
    irq_register(IDT_VEC_NMI, nmi);
    irq_register(IPI_TLB_VECTOR, tlb_ipi);
    irq_register(IPI_RESCHED_VECTOR, resched_ipi);
    if(!hal_irqchip_smp()){
        kprintf("[OK] smp cpus=1 (fara LAPIC)\n");
        return;
    }
    if(kernel_as.pml4_phys >= (1UL << 32)){
        panic("smp_init: PML4 la %lx, trampolina are nevoie de CR3 sub 4 GiB", kernel_as.pml4_phys);
    }
    size_t size = ap_trampoline_end - ap_trampoline_start;
    uint8_t* copy = (uint8_t*)phys_to_virt(TRAMPOLINE_PHYS);
    memcpy(copy, ap_trampoline_start, size);
    // Trampolina ruleaza la aceeasi adresa fizica si virtuala cand activeaza paginarea
    if(!vmm_map(kernel_as, TRAMPOLINE_PHYS, TRAMPOLINE_PHYS, PTE_RW)){
        panic("smp_init: nu pot mapa trampolina");
    }
    __asm__ volatile("mov %%cr0, %0" : "=r"(bsp_cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(bsp_cr4));
    uint32_t cr3 = (uint32_t)kernel_as.pml4_phys;
    uint32_t efer = (uint32_t)(rdmsr(MSR_EFER) & ~EFER_LMA);
    uint64_t entry = (uint64_t)ap_main;
    patch(copy, tr_cr3, &cr3, sizeof(cr3));
    patch(copy, tr_efer, &efer, sizeof(efer));
    patch(copy, tr_entry, &entry, sizeof(entry));

    uint32_t next = 1;
    for(int i = 0; i < acpi_madt.cpu_count; i++){
        const AcpiCpu& a = acpi_madt.cpus[i];
        if(!(a.flags & 1) || a.lapic_id == bsp.lapic_id){
            continue;
        }
        if(next >= (uint32_t)MAX_CPUS){
            kprintf("smp: ignor CPU lapic=%u (maxim %d)\n", a.lapic_id, MAX_CPUS);
            continue;
        }
        PerCpu& c = cpus[next];
        Task* idle = task_create_idle(next);
        uint64_t ist = alloc_pages(IST_ORDER);
        if(!idle || !ist){
            panic("smp_init: fara memorie pentru CPU %u", next);
        }
        c.id = next;
        c.lapic_id = a.lapic_id;
        c.idle = idle;
        c.ist1 = (uint8_t*)phys_to_virt(ist);
        c.kstack_top = (uint64_t)idle->kstack + TASK_KSTACK_SIZE;
        uint32_t id = next;
        patch(copy, tr_stack, &c.kstack_top, sizeof(c.kstack_top));
        patch(copy, tr_cpu, &id, sizeof(id));
        if(start_ap(c)){
            next++;
        } else {
            kprintf("smp: CPU lapic=%u nu a raspuns la INIT-SIPI-SIPI\n", a.lapic_id);
            task_destroy(idle);
            free_pages(ist, IST_ORDER);
            c = PerCpu{};
        }
    }
    vmm_unmap(kernel_as, TRAMPOLINE_PHYS);
    kprintf("[OK] smp cpus=%u\n", online_count);
}

void smp_send_resched(uint32_t cpu){
    PerCpu& c = cpus[cpu];
    if(c.online && cpu != cpu_id()){
        hal_irqchip()->send_ipi(c.lapic_id, IPI_RESCHED_VECTOR);
    }
}

uint32_t tlb_shootdown(uint64_t pml4_phys, uint64_t virt){
    if(online_count < 2){
        return 0;
    }
    // Jumatatea kernel e comuna tuturor spatiilor de adrese
    bool everywhere = virt >= KERNEL_HALF_START || pml4_phys == kernel_as.pml4_phys;
    SpinlockGuard guard(tlb_lock);
    tlb_virt = virt;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t me = cpu_id();
    uint32_t sent = 0;
    uint32_t mask = 0;
    for(uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++){
        PerCpu& c = cpus[i];
        if(i == me || !c.online || (!everywhere && __atomic_load_n(&c.cr3, __ATOMIC_ACQUIRE) != pml4_phys)){
            continue;
        }
        __atomic_store_n(&c.tlb_pending, true, __ATOMIC_RELEASE);
        hal_irqchip()->send_ipi(c.lapic_id, IPI_TLB_VECTOR);
        mask |= 1u << i;
        sent++;
    }
    uint32_t acked = 0;
    uint64_t deadline = monotonic_ns() + TLB_TIMEOUT_NS;
    for(uint32_t i = 0; i < (uint32_t)MAX_CPUS; i++){
        PerCpu& c = cpus[i];
        if(!(mask & (1u << i))){
            continue;
        }
        while(__atomic_load_n(&c.tlb_pending, __ATOMIC_ACQUIRE) && monotonic_ns() < deadline){
            cpu_relax();
        }
        if(__atomic_load_n(&c.tlb_pending, __ATOMIC_ACQUIRE)){
            kprintf("tlb_shootdown: CPU %u nu a confirmat %lx\n", i, virt);
            __atomic_store_n(&c.tlb_pending, false, __ATOMIC_RELEASE);
        } else {
            acked++;
        }
    }
    return acked;
}

void smp_spin_poll(){
    flush_pending(this_cpu());
}

void smp_stop_others(){
    if(online_count < 2 || stopping){
        return;
    }
    stopping = true;
    hal_irqchip()->nmi_others();
}
