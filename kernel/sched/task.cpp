#include "sched/task.h"
#include "sched/sched.h"
#include "sched/rt.h"
#include "proc/process.h"
#include "mm/slab.h"
#include "mm/buddy.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "sync/spinlock.h"
#include "arch/x86_64/percpu.h"
#include "init.h"

extern "C" char stack_bottom[];

namespace {

const int KSTACK_ORDER = 2;                 // 4 pagini = 16 KiB
const uint64_t RFLAGS_RESERVED = 1UL << 1;
static_assert(PAGE_SIZE << KSTACK_ORDER == TASK_KSTACK_SIZE, "stiva de kernel = 2^KSTACK_ORDER pagini");

KmemCache* task_cache = nullptr;
Spinlock tasks_lock("tasks");
ListLink all_tasks = {&all_tasks, &all_tasks};
uint64_t all_count = 0;
uint64_t next_tid = 0;

void link_add(ListLink& head, ListLink& l){
    l.prev = head.prev;
    l.next = &head;
    head.prev->next = &l;
    head.prev = &l;
}

void link_remove(ListLink& l){
    l.prev->next = l.next;
    l.next->prev = l.prev;
    l.prev = l.next = &l;
}

Task* new_task(const char* name, uint8_t prio){
    Task* t = (Task*)kmem_cache_alloc(task_cache);
    if(!t){
        return nullptr;
    }
    t->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    t->pid = 0;
    t->state = TaskState::READY;
    t->cls = TaskClass::NORMAL;
    t->prio = prio;
    t->ctx = Context{};
    t->kstack = nullptr;
    t->as = &kernel_as;
    t->slice_left = 0;
    t->wake_ns = 0;
    t->rt = nullptr;
    t->proc = nullptr;
    t->cpu = cpu_id();
    t->affinity = -1;
    t->on_cpu = false;
    int i = 0;
    for(; i < TASK_NAME_LEN - 1 && name[i]; i++){
        t->name[i] = name[i];
    }
    t->name[i] = 0;
    t->queue.prev = t->queue.next = &t->queue;
    SpinlockGuard lock(tasks_lock);
    link_add(all_tasks, t->all);
    all_count++;
    return t;
}

}

// Prima instructiune a unui thread nou: fn in r12, arg in r13 (puse de task_create_kernel).
// Porneste cu lock-ul scheduler-ului luat si intreruperile oprite; sched_task_start le elibereaza
__asm__(
    ".text\n"
    ".globl task_trampoline\n"
    "task_trampoline:\n"
    "    call sched_task_start\n"
    "    mov %r13, %rdi\n"
    "    call *%r12\n"
    "    call task_exit_current\n"
    "    ud2\n");

extern "C" void task_trampoline();

extern "C" [[noreturn]] void task_exit_current(){
    task_exit();
}

__init void tasks_init(){
    task_cache = kmem_cache_create("task", sizeof(Task), alignof(Task));
    if(!task_cache){
        panic("tasks_init: nu pot crea cache-ul de Task");
    }
    // Task 0 continua pe stiva de boot; contextul se completeaza la prima comutare
    Task* idle = new_task("idle", 0);
    if(!idle || idle->tid != 0){
        panic("tasks_init: task 0 nu a putut fi creat");
    }
    idle->state = TaskState::RUNNING;
    idle->kstack = (uint8_t*)stack_bottom;
    idle->affinity = 0;
    idle->on_cpu = true;
    this_cpu()->current = idle;
    this_cpu()->idle = idle;
    kprintf("[OK] tasks (task 0 \"%s\", Task = %lu bytes)\n", idle->name, sizeof(Task));
}

namespace {

Task* with_stack(const char* name, uint8_t prio){
    uint64_t phys = alloc_pages(KSTACK_ORDER);
    if(!phys){
        return nullptr;
    }
    Task* t = new_task(name, prio);
    if(!t){
        free_pages(phys, KSTACK_ORDER);
        return nullptr;
    }
    t->kstack = (uint8_t*)phys_to_virt(phys);
    return t;
}

}

Task* task_create_idle(uint32_t cpu){
    char name[TASK_NAME_LEN] = "idle";
    name[4] = (char)('0' + cpu / 10);
    name[5] = (char)('0' + cpu % 10);
    Task* t = with_stack(name, 0);
    if(t){
        t->cpu = cpu;
        t->affinity = (int32_t)cpu;
    }
    return t;
}

Task* task_create_kernel(const char* name, TaskFn fn, void* arg, uint8_t prio){
    Task* t = with_stack(name, prio);
    if(!t){
        return nullptr;
    }
    // rsp aliniat la 16: trampolina face call, deci fn vede alinierea din ABI
    t->ctx.rsp = (uint64_t)(t->kstack + TASK_KSTACK_SIZE);
    t->ctx.rip = (uint64_t)task_trampoline;
    t->ctx.rflags = RFLAGS_RESERVED;            // IF se activeaza in sched_task_start
    t->ctx.r12 = (uint64_t)fn;
    t->ctx.r13 = (uint64_t)arg;
    return t;
}

void task_destroy(Task* t){
    if(t == task_current() || t->state == TaskState::RUNNING || t->on_cpu){
        panic("task_destroy: task %lu (%s) ruleaza", t->tid, t->name);
    }
    rt_task_destroyed(t);
    process_task_destroyed(t);
    if(t->as != &kernel_as){
        as_destroy(*t->as);
        kfree(t->as);
    }
    free_pages((uint64_t)t->kstack - phys_map_base, KSTACK_ORDER);
    // Ultimul pas: cine asteapta task_count() (testele, pe alt CPU) vede memoria deja eliberata
    {
        SpinlockGuard lock(tasks_lock);
        link_remove(t->all);
        if(t->queue.next != &t->queue){
            link_remove(t->queue);
        }
        all_count--;
    }
    kmem_cache_free(task_cache, t);
}

Task* task_current(){
    Task* t;
    __asm__ volatile("mov %%gs:24, %0" : "=r"(t));
    return t;
}

uint64_t task_count(){
    return all_count;
}
