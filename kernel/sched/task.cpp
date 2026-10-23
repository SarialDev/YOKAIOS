#include "sched/task.h"
#include "mm/slab.h"
#include "mm/buddy.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "irqlock.h"
#include "init.h"

extern "C" char stack_bottom[];

namespace {

const int KSTACK_ORDER = 2;                 // 4 pagini = 16 KiB
const uint64_t RFLAGS_IF = 1UL << 9;
const uint64_t RFLAGS_RESERVED = 1UL << 1;
static_assert(PAGE_SIZE << KSTACK_ORDER == TASK_KSTACK_SIZE, "stiva de kernel = 2^KSTACK_ORDER pagini");

KmemCache* task_cache = nullptr;
Task* current = nullptr;
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
    int i = 0;
    for(; i < TASK_NAME_LEN - 1 && name[i]; i++){
        t->name[i] = name[i];
    }
    t->name[i] = 0;
    t->queue.prev = t->queue.next = &t->queue;
    IrqLock lock;
    link_add(all_tasks, t->all);
    all_count++;
    return t;
}

}

// Prima instructiune a unui thread nou: fn in r12, arg in r13 (puse de task_create_kernel)
__asm__(
    ".text\n"
    ".globl task_trampoline\n"
    "task_trampoline:\n"
    "    mov %r13, %rdi\n"
    "    call *%r12\n"
    "    call task_exit_current\n"
    "    ud2\n");

extern "C" void task_trampoline();

extern "C" [[noreturn]] void task_exit_current(){
    // T4.2/T4.4: marcheaza ZOMBIE si comuta pe alt task
    panic("task %lu (%s) a iesit, dar comutarea vine in T4.2", current->tid, current->name);
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
    current = idle;
    kprintf("[OK] tasks (task 0 \"%s\", Task = %lu bytes)\n", idle->name, sizeof(Task));
}

Task* task_create_kernel(const char* name, TaskFn fn, void* arg, uint8_t prio){
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
    // rsp aliniat la 16: trampolina face call, deci fn vede alinierea din ABI
    t->ctx.rsp = (uint64_t)(t->kstack + TASK_KSTACK_SIZE);
    t->ctx.rip = (uint64_t)task_trampoline;
    t->ctx.rflags = RFLAGS_IF | RFLAGS_RESERVED;
    t->ctx.r12 = (uint64_t)fn;
    t->ctx.r13 = (uint64_t)arg;
    return t;
}

void task_destroy(Task* t){
    if(t == current || t->state == TaskState::RUNNING){
        panic("task_destroy: task %lu (%s) ruleaza", t->tid, t->name);
    }
    {
        IrqLock lock;
        link_remove(t->all);
        if(t->queue.next != &t->queue){
            link_remove(t->queue);
        }
        all_count--;
    }
    free_pages((uint64_t)t->kstack - phys_map_base, KSTACK_ORDER);
    kmem_cache_free(task_cache, t);
}

Task* task_current(){
    return current;
}

uint64_t task_count(){
    return all_count;
}
