#include "sched/sched.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/lapic.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "irqlock.h"

extern "C" void switch_to(Context* old_ctx, Context* new_ctx);

// switch.S foloseste offset-uri fixe in Context
static_assert(__builtin_offsetof(Context, rsp) == 0 && __builtin_offsetof(Context, rip) == 8 &&
              __builtin_offsetof(Context, rflags) == 16 && __builtin_offsetof(Context, rbx) == 24 &&
              __builtin_offsetof(Context, r15) == 64, "Context trebuie sa corespunda cu switch.S");

namespace {

const uint32_t SLICE_TICKS = SCHED_TIMESLICE_MS * TIMER_HZ / 1000;

Task* idle_task = nullptr;
ListLink runqueue = {&runqueue, &runqueue};     // READY, FIFO
ListLink sleepers = {&sleepers, &sleepers};     // BLOCKED, sortate dupa wake_ns
ListLink zombies = {&zombies, &zombies};        // ZOMBIE, de eliberat
volatile bool need_resched = false;

Task* task_of(ListLink* l){
    return (Task*)((uint8_t*)l - __builtin_offsetof(Task, queue));
}

bool empty(const ListLink& head){
    return head.next == &head;
}

void push_back(ListLink& head, Task* t){
    ListLink& l = t->queue;
    l.prev = head.prev;
    l.next = &head;
    head.prev->next = &l;
    head.prev = &l;
}

void unlink(Task* t){
    ListLink& l = t->queue;
    l.prev->next = l.next;
    l.next->prev = l.prev;
    l.prev = l.next = &l;
}

Task* pop_front(ListLink& head){
    if(empty(head)){
        return nullptr;
    }
    Task* t = task_of(head.next);
    unlink(t);
    return t;
}

// Insereaza dupa ultimul task care se trezeste mai devreme sau odata cu t
void insert_sleeper(Task* t){
    ListLink* pos = &sleepers;
    while(pos->prev != &sleepers && task_of(pos->prev)->wake_ns > t->wake_ns){
        pos = pos->prev;
    }
    ListLink& l = t->queue;
    l.next = pos;
    l.prev = pos->prev;
    pos->prev->next = &l;
    pos->prev = &l;
}

// Task-urile terminate nu mai ruleaza, deci stiva lor poate fi eliberata de oricine altcineva
void reap(){
    while(!empty(zombies) && task_of(zombies.next) != task_current()){
        task_destroy(pop_front(zombies));
    }
}

uint64_t kstack_top(Task* t){
    return (uint64_t)t->kstack + TASK_KSTACK_SIZE;
}

// Alege urmatorul task si comuta pe el; se apeleaza cu intreruperile oprite
void schedule(){
    need_resched = false;
    Task* prev = task_current();
    if(prev->state == TaskState::RUNNING){
        prev->state = TaskState::READY;
        if(prev != idle_task){
            push_back(runqueue, prev);
        }
    }
    Task* next = pop_front(runqueue);
    if(!next){
        next = idle_task;
    }
    next->state = TaskState::RUNNING;
    next->slice_left = SLICE_TICKS;
    if(next == prev){
        return;
    }
    if(next->as->pml4_phys != prev->as->pml4_phys){
        as_switch(*next->as);
    }
    tss_set_rsp0(kstack_top(next));
    task_set_current(next);
    switch_to(&prev->ctx, &next->ctx);
    // Aici se revine cand prev e ales din nou
}

}

void sched_init(){
    idle_task = task_current();
    if(!idle_task || idle_task->tid != 0){
        panic("sched_init: task 0 lipseste");
    }
    idle_task->slice_left = SLICE_TICKS;
    tss_set_rsp0(kstack_top(idle_task));
    kprintf("[OK] sched (round-robin, timeslice %u ms)\n", SCHED_TIMESLICE_MS);
}

void sched_add(Task* t){
    IrqLock lock;
    t->state = TaskState::READY;
    push_back(runqueue, t);
}

void yield(){
    IrqLock lock;
    reap();
    schedule();
}

void task_exit(){
    __asm__ volatile("cli");
    Task* t = task_current();
    if(t == idle_task){
        panic("task_exit: task 0 (idle) nu poate iesi");
    }
    t->state = TaskState::ZOMBIE;
    push_back(zombies, t);
    schedule();
    panic("task_exit: task-ul %lu a revenit dupa ZOMBIE", t->tid);
}

bool sched_sleep_until(uint64_t wake_ns){
    Task* t = task_current();
    if(t == idle_task || !lapic_enabled()){
        return false;
    }
    IrqLock lock;
    t->wake_ns = wake_ns;
    t->state = TaskState::BLOCKED;
    insert_sleeper(t);
    schedule();
    return true;
}

void sched_tick(){
    uint64_t now = monotonic_ns();
    while(!empty(sleepers) && task_of(sleepers.next)->wake_ns <= now){
        Task* t = pop_front(sleepers);
        t->state = TaskState::READY;
        push_back(runqueue, t);
    }
    Task* cur = task_current();
    if(cur == idle_task){
        if(!empty(runqueue)){
            need_resched = true;
        }
    } else if(cur->slice_left == 0 || --cur->slice_left == 0){
        need_resched = true;
    }
}

void sched_irq_exit(){
    if(need_resched){
        schedule();
    }
}

void sched_idle(){
    __asm__ volatile("sti");
    for(;;){
        {
            IrqLock lock;
            reap();
            if(!empty(runqueue)){
                schedule();
            }
        }
        __asm__ volatile("hlt");
    }
}
