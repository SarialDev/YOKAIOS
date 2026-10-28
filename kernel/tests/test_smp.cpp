#include "tests/test_smp.h"
#include "arch/x86_64/smp.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sync/atomic.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const int PIN_ROUNDS = 20;
const uint8_t PIN_PRIO = 40;
const uint64_t SPIN_NS = 20000000;              // 20 ms per task la load balancing
const uint64_t TEST_VIRT = 0xFFFFA00000200000UL;  // in afara HHDM, a kernelului si a ferestrei MMIO
const uint64_t OLD_VALUE = 0x01D01D01D01D01DUL;
const uint64_t NEW_VALUE = 0x4E3E4E3E4E3E4E3EUL;

Atomic<int> finished(0);

// Fara POPCNT in -march=x86-64, iar __builtin_popcount ar cere libgcc
int bit_count(uint32_t v){
    int n = 0;
    for(; v; v &= v - 1){
        n++;
    }
    return n;
}

// Task 0 (idle) nu are voie sa se blocheze, deci asteapta cu yield + hlt
void wait_finished(int n){
    while(finished.load() < n){
        yield();
        __asm__ volatile("hlt");
    }
    yield();                                    // elibereaza task-urile terminate
    finished.store(0);
}

Task* start(const char* name, TaskFn fn, void* arg, int32_t cpu = -1, TaskClass cls = TaskClass::NORMAL,
            uint8_t prio = 0){
    Task* t = task_create_kernel(name, fn, arg, prio);
    if(!t){
        panic("test_smp: task_create_kernel(%s) a esuat", name);
    }
    t->cls = cls;
    if(cpu >= 0 && !sched_set_affinity(t, cpu)){
        panic("test_smp: sched_set_affinity(%s, %d) a esuat", name, cpu);
    }
    sched_add(t);
    return t;
}

// RT fixat pe un CPU: fiecare trezire (eliberata de oricare CPU) trebuie sa ruleze tot acolo
Atomic<uint32_t> pinned_mask(0);

void pinned_thread(void*){
    for(int i = 0; i < PIN_ROUNDS; i++){
        pinned_mask.fetch_or(1u << cpu_id());
        sleep_ms(1);
    }
    finished.fetch_add(1);
}

// Se muta singur pe fiecare CPU pe rand
volatile int migrate_errors = 0;

void migrate_thread(void*){
    for(uint32_t cpu = 0; cpu < smp_cpu_count(); cpu++){
        sched_set_affinity(task_current(), (int32_t)cpu);
        if(cpu_id() != cpu){
            migrate_errors++;
        }
    }
    sched_set_affinity(task_current(), -1);
    finished.fetch_add(1);
}

void test_affinity(){
    uint32_t target = smp_cpu_count() - 1;
    start("pinned", pinned_thread, nullptr, (int32_t)target, TaskClass::RT, PIN_PRIO);
    start("migrate", migrate_thread, nullptr);
    wait_finished(2);
    if(pinned_mask.load() != 1u << target || migrate_errors){
        panic("test_smp affinity: RT pe CPU-urile %x (asteptat %x), %d migrari gresite", pinned_mask.load(),
              1u << target, migrate_errors);
    }
    kprintf("[OK] test_smp affinity (RT fixat pe CPU %u, migrare pe %u CPU-uri)\n", target, smp_cpu_count());
}

// Toate pornesc in coada CPU-ului 1; celelalte CPU-uri, libere, trebuie sa le ia
Atomic<uint32_t> spin_mask(0);

void spin_thread(void*){
    uint64_t end = monotonic_ns() + SPIN_NS;
    while(monotonic_ns() < end){
        spin_mask.fetch_or(1u << cpu_id());
    }
    finished.fetch_add(1);
}

void test_pull(){
    uint32_t n = smp_cpu_count();
    int tasks = (int)(2 * n);
    uint64_t before = sched_migrations();
    Task* all[2 * MAX_CPUS];
    for(int i = 0; i < tasks; i++){
        all[i] = start("spin", spin_thread, nullptr, 1);
    }
    // Raman in coada CPU-ului 1, dar pot fi luate de oricine
    for(int i = 0; i < tasks; i++){
        sched_set_affinity(all[i], -1);
    }
    wait_finished(tasks);
    uint64_t moved = sched_migrations() - before;
    int used = bit_count(spin_mask.load());
    if(moved == 0 || used < (int)(n > 3 ? 3 : n)){
        panic("test_smp pull: %lu migrari, %d CPU-uri folosite (masca %x)", moved, used, spin_mask.load());
    }
    kprintf("[OK] test_smp pull (%d task-uri din coada CPU 1 -> %d CPU-uri, %lu luate la idle)\n", tasks, used,
            moved);
}

// Cititori pe celelalte CPU-uri: au in TLB maparea veche; dupa remapare trebuie sa vada pagina noua
volatile int phase = 0;
volatile uint64_t seen_old[MAX_CPUS];
volatile uint64_t seen_new[MAX_CPUS];
Atomic<int> ready(0);

void reader_thread(void*){
    uint32_t me = cpu_id();
    seen_old[me] = *(volatile uint64_t*)TEST_VIRT;
    ready.fetch_add(1);
    while(phase == 0){
        cpu_relax();
    }
    seen_new[me] = *(volatile uint64_t*)TEST_VIRT;
    finished.fetch_add(1);
}

void test_tlb(){
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    if(!a || !b){
        panic("test_smp tlb: fara memorie");
    }
    *(volatile uint64_t*)phys_to_virt(a) = OLD_VALUE;
    *(volatile uint64_t*)phys_to_virt(b) = NEW_VALUE;
    if(!vmm_map(kernel_as, TEST_VIRT, a, PTE_RW | PTE_NX)){
        panic("test_smp tlb: vmm_map a esuat");
    }
    phase = 0;
    ready.store(0);
    int readers = (int)smp_cpu_count() - 1;
    for(int i = 1; i <= readers; i++){
        start("reader", reader_thread, nullptr, i);
    }
    while(ready.load() < readers){
        yield();
        __asm__ volatile("hlt");
    }
    // Fara shootdown, cititorii ar citi in continuare pagina a din TLB-ul lor
    vmm_unmap(kernel_as, TEST_VIRT);
    if(!vmm_map(kernel_as, TEST_VIRT, b, PTE_RW | PTE_NX)){
        panic("test_smp tlb: remaparea a esuat");
    }
    phase = 1;
    wait_finished(readers);
    for(int i = 1; i <= readers; i++){
        if(seen_old[i] != OLD_VALUE || seen_new[i] != NEW_VALUE){
            panic("test_smp tlb: CPU %d a citit %lx apoi %lx (asteptat %lx apoi %lx)", i, seen_old[i], seen_new[i],
                  OLD_VALUE, NEW_VALUE);
        }
    }
    vmm_unmap(kernel_as, TEST_VIRT);
    pmm_free_page(a);
    pmm_free_page(b);
    kprintf("[OK] test_smp tlb (shootdown la unmap, %d CPU-uri vad maparea noua)\n", readers);
}

}

void test_smp_run(){
    __asm__ volatile("sti");
    if(smp_cpu_count() < 2){
        kprintf("[OK] test_smp (un singur CPU, nimic de testat)\n");
        __asm__ volatile("cli");
        return;
    }
    uint64_t count_before = task_count();
    test_affinity();
    test_pull();
    test_tlb();
    if(task_count() != count_before){
        panic("test_smp: %lu task-uri ramase, %lu inainte", task_count(), count_before);
    }
    uint64_t ipis = 0;
    for(uint32_t i = 0; i < smp_cpu_count(); i++){
        ipis += cpus[i].ipis;
    }
    __asm__ volatile("cli");
    kprintf("[OK] test_smp (%u CPU-uri, %lu IPI-uri de reschedule)\n", smp_cpu_count(), ipis);
}
