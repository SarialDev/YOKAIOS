#include "tests/test_cpu.h"
#include "interrupt.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "time/clock.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "sched/task.h"

namespace {

const uint64_t VEC_DE = 0;
const uint64_t VEC_PF = 14;
const uint64_t UNMAPPED_ADDR = 0x1000;     // harta identitate 0-1 GiB e stearsa in boot.S
const int MONOTONIC_READS = 10000;
const uint32_t SLEEP_MS = 100;
const uint64_t SLEEP_MIN_NS = 95000000;
const uint64_t SLEEP_MAX_NS = 105000000;
const uint64_t PF_PRESENT = 1 << 0;
const uint64_t PF_WRITE = 1 << 1;
const uint64_t PF_FETCH = 1 << 4;
const uint64_t TEST_USER_VIRT = 0x400000;
const int HEAP_ROUNDS = 1000;
const int HEAP_LIVE = 64;

// Obiecte de marimi diferite pentru testul de heap; constructorul marcheaza memoria
template<int N>
struct Blob {
    uint8_t bytes[N];
    Blob(){ bytes[0] = (uint8_t)N; bytes[N - 1] = 0xA5; }
    bool intact() const { return bytes[0] == (uint8_t)N && bytes[N - 1] == 0xA5; }
};

// Un singur ret intr-o pagina RW+NX (.data): executia lui trebuie sa dea #PF
volatile uint8_t data_code[16] = {0xC3};

volatile uint64_t resume_rip = 0;
volatile uint64_t seen_vector = ~0UL;
volatile uint64_t seen_error = 0;
volatile uint64_t seen_cr2 = 0;

// Inregistreaza exceptia si sare peste instructiunea care a generat-o
void record_and_skip(InterruptFrame* f){
    seen_vector = f->vector;
    seen_error = f->error;
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    seen_cr2 = cr2;
    f->rip = resume_rip;
}

void reset(){
    seen_vector = ~0UL;
    seen_error = 0;
    seen_cr2 = 0;
}

void trigger_de(){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "xor %%ecx, %%ecx\n\t"
        "mov $1, %%eax\n\t"
        "xor %%edx, %%edx\n\t"
        "div %%ecx\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : : "eax", "ecx", "edx", "memory");
}

void trigger_pf(){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "mov (%2), %%eax\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : "r"(UNMAPPED_ADDR) : "eax", "memory");
}

void trigger_text_write(uint64_t addr){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "movb $0x90, (%2)\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : "r"(addr) : "memory");
}

// jmp (nu call) ca stiva sa ramana neschimbata cand handlerul sare la eticheta
void trigger_nx_exec(uint64_t addr){
    uint64_t tmp;
    __asm__ volatile(
        "lea 1f(%%rip), %0\n\t"
        "mov %0, %1\n\t"
        "jmp *%2\n"
        "1:"
        : "=&r"(tmp), "=m"(resume_rip) : "r"(addr) : "memory");
}

void test_exceptions(){
    InterruptHandler old_de = irq_register(VEC_DE, record_and_skip);
    InterruptHandler old_pf = irq_register(VEC_PF, record_and_skip);

    reset();
    trigger_de();
    if(seen_vector != VEC_DE){
        panic("test_exceptions: #DE asteptat, vector=%lx", seen_vector);
    }

    reset();
    trigger_pf();
    if(seen_vector != VEC_PF || seen_cr2 != UNMAPPED_ADDR){
        panic("test_exceptions: #PF asteptat, vector=%lx cr2=%lx", seen_vector, seen_cr2);
    }

    irq_register(VEC_DE, old_de);
    irq_register(VEC_PF, old_pf);
    kprintf("[OK] test_exceptions\n");
}

void test_paging(){
    InterruptHandler old_pf = irq_register(VEC_PF, record_and_skip);

    uint64_t text = (uint64_t)&test_cpu_run;
    reset();
    trigger_text_write(text);
    if(seen_vector != VEC_PF || seen_cr2 != text || (seen_error & (PF_PRESENT | PF_WRITE)) != (PF_PRESENT | PF_WRITE)){
        panic("test_paging: scriere in .text fara #PF (vector=%lx err=%lx)", seen_vector, seen_error);
    }

    uint64_t data = (uint64_t)data_code;
    reset();
    trigger_nx_exec(data);
    if(seen_vector != VEC_PF || seen_cr2 != data || !(seen_error & PF_FETCH)){
        panic("test_paging: executie din .data fara #PF (vector=%lx err=%lx)", seen_vector, seen_error);
    }
    irq_register(VEC_PF, old_pf);

    // Spatiu de adrese nou: pagina proprie jos, kernelul vizibil sus
    AddressSpace as = as_create();
    uint64_t page = pmm_alloc_page();
    if(!as.pml4_phys || !page || !vmm_map(as, TEST_USER_VIRT, page, PTE_RW | PTE_USER | PTE_NX)){
        panic("test_paging: as_create/vmm_map a esuat");
    }
    as_switch(as);
    *(volatile uint64_t*)TEST_USER_VIRT = 0x59AB;
    as_switch(kernel_as);
    uint64_t seen = *(volatile uint64_t*)phys_to_virt(page);
    if(seen != 0x59AB || vmm_translate(as, TEST_USER_VIRT) != page || vmm_translate(kernel_as, TEST_USER_VIRT)){
        panic("test_paging: spatiul de adrese nou nu e izolat (val=%lx)", seen);
    }
    vmm_unmap(as, TEST_USER_VIRT);
    if(vmm_translate(as, TEST_USER_VIRT)){
        panic("test_paging: vmm_unmap nu a sters maparea");
    }
    pmm_free_page(page);
    kprintf("[OK] test_paging\n");
}

void test_heap(){
    // Cache dedicat, ca pentru Task/inode
    KmemCache* cache = kmem_cache_create("test-obj", 40, 8);
    void* a = kmem_cache_alloc(cache);
    void* b = kmem_cache_alloc(cache);
    if(!a || !b || a == b || kmem_cache_object_size(cache) < 40){
        panic("test_heap: kmem_cache_create/alloc a esuat");
    }
    kmem_cache_free(cache, a);
    kmem_cache_free(cache, b);

    uint64_t free_before = pmm_free_count();
    void* live[HEAP_LIVE] = {};
    int kinds[HEAP_LIVE] = {};
    uint32_t seed = 12345;
    for(int i = 0; i < HEAP_ROUNDS; i++){
        seed = seed * 1103515245 + 12345;
        int slot = (seed >> 16) % HEAP_LIVE;
        // Elibereaza ce era in slot, verificand ca obiectul nu a fost suprascris
        switch(kinds[slot]){
            case 1: if(!((Blob<24>*)live[slot])->intact()) panic("test_heap: Blob<24> corupt"); delete (Blob<24>*)live[slot]; break;
            case 2: if(!((Blob<200>*)live[slot])->intact()) panic("test_heap: Blob<200> corupt"); delete (Blob<200>*)live[slot]; break;
            case 3: if(!((Blob<1500>*)live[slot])->intact()) panic("test_heap: Blob<1500> corupt"); delete (Blob<1500>*)live[slot]; break;
            case 4: delete[] (uint64_t*)live[slot]; break;
            case 5: kfree(live[slot]); break;
        }
        int kind = 1 + (seed >> 8) % 5;
        switch(kind){
            case 1: live[slot] = new Blob<24>; break;
            case 2: live[slot] = new Blob<200>; break;
            case 3: live[slot] = new Blob<1500>; break;
            case 4: live[slot] = new uint64_t[700]; break;     // 5600 bytes: din buddy
            case 5: {
                uint8_t* p = (uint8_t*)kzalloc(100);
                for(int k = 0; k < 100; k++){
                    if(p[k]) panic("test_heap: kzalloc nu a pus zero");
                    p[k] = (uint8_t)k;
                }
                p = (uint8_t*)krealloc(p, 3000);
                for(int k = 0; k < 100; k++){
                    if(p[k] != (uint8_t)k) panic("test_heap: krealloc a pierdut datele");
                }
                live[slot] = p;
                break;
            }
        }
        kinds[slot] = kind;
    }
    for(int slot = 0; slot < HEAP_LIVE; slot++){
        switch(kinds[slot]){
            case 1: delete (Blob<24>*)live[slot]; break;
            case 2: delete (Blob<200>*)live[slot]; break;
            case 3: delete (Blob<1500>*)live[slot]; break;
            case 4: delete[] (uint64_t*)live[slot]; break;
            case 5: kfree(live[slot]); break;
        }
    }
    uint64_t free_after = pmm_free_count();
    if(free_after != free_before){
        panic("test_heap: memoria folosita a crescut (%lu -> %lu pagini libere)", free_before, free_after);
    }
    kprintf("[OK] test_heap (%d operatii, %lu pagini libere inainte si dupa)\n", HEAP_ROUNDS, free_after);
}

void dummy_thread(void*){
}

void test_tasks(){
    Task* idle = task_current();
    if(!idle || idle->tid != 0 || idle->state != TaskState::RUNNING){
        panic("test_tasks: task 0 lipseste sau nu ruleaza");
    }
    const int N = 3;
    Task* t[N];
    uint64_t count_before = task_count();
    uint64_t free_before = pmm_free_count();
    int marker = 0;
    for(int i = 0; i < N; i++){
        t[i] = task_create_kernel("worker", dummy_thread, &marker, (uint8_t)i);
        if(!t[i] || t[i]->state != TaskState::READY || t[i]->as != &kernel_as || t[i]->pid != 0){
            panic("test_tasks: task_create_kernel a esuat la %d", i);
        }
        uint64_t top = (uint64_t)t[i]->kstack + TASK_KSTACK_SIZE;
        if(t[i]->ctx.rsp != top || (t[i]->ctx.rsp & 15) || !t[i]->ctx.rip ||
           t[i]->ctx.r12 != (uint64_t)dummy_thread || t[i]->ctx.r13 != (uint64_t)&marker){
            panic("test_tasks: context initial gresit pentru tid %lu", t[i]->tid);
        }
        if(i && t[i]->tid <= t[i - 1]->tid){
            panic("test_tasks: tid-uri necrescatoare (%lu dupa %lu)", t[i]->tid, t[i - 1]->tid);
        }
        // Stiva trebuie sa fie scriibila in intregime
        t[i]->kstack[0] = 1;
        t[i]->kstack[TASK_KSTACK_SIZE - 1] = 1;
    }
    if(task_count() != count_before + N){
        panic("test_tasks: task_count %lu, asteptat %lu", task_count(), count_before + N);
    }
    uint64_t first = t[0]->tid;
    for(int i = 0; i < N; i++){
        task_destroy(t[i]);
    }
    if(task_count() != count_before || pmm_free_count() != free_before){
        panic("test_tasks: task_destroy nu a eliberat tot (task-uri %lu, pagini %lu -> %lu)",
              task_count(), free_before, pmm_free_count());
    }
    kprintf("[OK] test_tasks (%d thread-uri, tid %lu..%lu)\n", N, first, first + N - 1);
}

// noinline + index volatil: compilatorul nu poate dovedi depasirea, deci nu o elimina si nu avertizeaza
__attribute__((noinline)) void overflow_buffer(volatile int count){
    char buf[16];
    for(int i = 0; i < count; i++){
        buf[i] = 'A';
    }
    __asm__ volatile("" : : "r"(buf) : "memory");
}

void test_timer(){
    __asm__ volatile("sti");
    uint64_t prev = monotonic_ns();
    for(int i = 0; i < MONOTONIC_READS; i++){
        uint64_t now = monotonic_ns();
        if(now <= prev){
            panic("test_timer: monotonic_ns nu creste la citirea %d (%lu <= %lu)", i, now, prev);
        }
        prev = now;
    }

    uint64_t start = monotonic_ns();
    sleep_ms(SLEEP_MS);
    uint64_t slept = monotonic_ns() - start;
    __asm__ volatile("cli");
    if(slept < SLEEP_MIN_NS || slept > SLEEP_MAX_NS){
        panic("test_timer: sleep_ms(%u) a durat %lu ns", SLEEP_MS, slept);
    }
    kprintf("[OK] test_timer (sleep_ms(%u) = %lu us)\n", SLEEP_MS, slept / 1000);
}

}

void test_cpu_run(){
    test_exceptions();
    test_paging();
    test_heap();
    test_tasks();
    test_timer();
}

void test_stack_smash(){
    kprintf("test_stack_smash: scriu 32 de bytes intr-un buffer de 16\n");
    overflow_buffer(32);
    panic("test_stack_smash: depasirea NU a fost detectata");
}
