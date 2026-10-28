#include "sched/sched.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/lapic.h"
#include "syscall/syscall.h"
#include "sched/rt.h"
#include "sched/watchdog.h"
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

const uint64_t RFLAGS_IF = 1UL << 9;

Task* idle_task = nullptr;
ListLink runqueue = {&runqueue, &runqueue};     // NORMAL READY, FIFO (RT: in sched/rt.cpp)
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

bool is_rt(const Task* t){
    return t->cls == TaskClass::RT;
}

// RT in coada pe prioritate, NORMAL la coada FIFO
void enqueue(Task* t){
    if(is_rt(t)){
        rt_enqueue(t);
    } else {
        push_back(runqueue, t);
    }
}

bool any_ready(){
    return rt_top_prio() || !empty(runqueue);
}

// t tocmai a devenit READY: preemptie daca e RT si mai important decat ce ruleaza
void check_preempt(const Task* t){
    Task* cur = task_current();
    if(cur == idle_task || (is_rt(t) && (!is_rt(cur) || t->prio > cur->prio))){
        need_resched = true;
    }
}

bool interrupts_enabled(){
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    return rflags & RFLAGS_IF;
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
    if(sleepers.next == &l){
        clock_reprogram();                      // noul cel mai apropiat eveniment
    }
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
            enqueue(prev);
        }
    }
    Task* next = rt_pick();
    if(!next){
        next = pop_front(runqueue);
    }
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
    syscall_set_kstack(kstack_top(next));
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
    kprintf("[OK] sched (RT %u-%u peste NORMAL round-robin, timeslice %u ms)\n", RT_PRIO_MIN, RT_PRIO_MAX,
            SCHED_TIMESLICE_MS);
}

void sched_add(Task* t){
    bool can_switch = interrupts_enabled();
    IrqLock lock;
    t->state = TaskState::READY;
    enqueue(t);
    check_preempt(t);
    // Un task RT mai important ruleaza imediat; in IRQ sau sub lock se asteapta iesirea din IRQ
    if(is_rt(t) && need_resched && can_switch){
        schedule();
    }
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

void sched_block(){
    if(task_current() == idle_task){
        panic("sched_block: task 0 (idle) nu are voie sa se blocheze");
    }
    schedule();
}

void sched_wake(Task* t){
    bool can_switch = interrupts_enabled();
    IrqLock lock;
    if(t->state != TaskState::BLOCKED){
        return;
    }
    t->state = TaskState::READY;
    enqueue(t);
    check_preempt(t);
    if(is_rt(t) && need_resched && can_switch){
        schedule();
    }
}

void sched_set_class(Task* t, TaskClass cls, uint8_t prio){
    IrqLock lock;
    bool queued = t->state == TaskState::READY && t->queue.next != &t->queue;
    if(queued){
        if(is_rt(t)){
            rt_remove(t);
        } else {
            unlink(t);
        }
    }
    t->cls = cls;
    t->prio = prio;
    if(queued){
        enqueue(t);
        check_preempt(t);
    }
}

void sched_timer_event(uint64_t now){
    while(!empty(sleepers) && task_of(sleepers.next)->wake_ns <= now){
        Task* t = pop_front(sleepers);
        t->state = TaskState::READY;
        enqueue(t);
        check_preempt(t);
    }
}

uint64_t sched_next_wake(){
    return empty(sleepers) ? SCHED_NO_WAKE : task_of(sleepers.next)->wake_ns;
}

void sched_tick(uint64_t now){
    watchdog_tick(now);
    Task* cur = task_current();
    if(cur == idle_task){
        if(any_ready()){
            need_resched = true;
        }
    } else if(is_rt(cur)){
        // RT: fara timeslice; cedeaza doar unui RT mai important sau cand se blocheaza
        if(rt_top_prio() > cur->prio){
            need_resched = true;
        }
    } else if(cur->slice_left == 0 || --cur->slice_left == 0){
        need_resched = true;
    }
}

void sched_irq_exit(){
    // Task periodic oprit de watchdog in timp ce rula (ex. bucla infinita): iese aici, dupa EOI
    Task* cur = task_current();
    if(cur->rt && cur->rt->stop){
        task_exit();
    }
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
            if(any_ready()){
                schedule();
            }
        }
        __asm__ volatile("hlt");
    }
}
