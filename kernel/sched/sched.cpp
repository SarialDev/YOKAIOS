#include "sched/sched.h"
#include "arch/x86_64/gdt.h"
#include "hal/irqchip.h"
#include "arch/x86_64/smp.h"
#include "syscall/syscall.h"
#include "sched/rt.h"
#include "sched/watchdog.h"
#include "sync/spinlock.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"

extern "C" void switch_to(Context* old_ctx, Context* new_ctx);

// switch.S foloseste offset-uri fixe in Context
static_assert(__builtin_offsetof(Context, rsp) == 0 && __builtin_offsetof(Context, rip) == 8 &&
              __builtin_offsetof(Context, rflags) == 16 && __builtin_offsetof(Context, rbx) == 24 &&
              __builtin_offsetof(Context, r15) == 64, "Context trebuie sa corespunda cu switch.S");

namespace {

const uint32_t SLICE_TICKS = SCHED_TIMESLICE_MS * TIMER_HZ / 1000;

const uint64_t RFLAGS_IF = 1UL << 9;

// Coada de rulare a unui CPU: RT pe prioritate, NORMAL FIFO
struct RunQueue {
    ListLink normal;
    RtQueue rt;
    uint32_t ready;                             // task-uri READY in coada (NORMAL + RT)
};

// Un singur lock pentru toate cozile, sleepers si zombies. E tinut peste switch_to: il elibereaza
// task-ul pe care s-a comutat (dupa schedule() sau in sched_task_start), deci un task pus in coada
// nu poate fi luat de alt CPU pana nu i s-a salvat contextul
Spinlock sched_lock("sched");
RunQueue rqs[MAX_CPUS];
ListLink sleepers = {&sleepers, &sleepers};     // BLOCKED, sortate dupa wake_ns (comune tuturor CPU-urilor)
ListLink zombies = {&zombies, &zombies};        // ZOMBIE, de eliberat
volatile uint64_t next_wake = SCHED_NO_WAKE;    // wake_ns al primului sleeper, citit fara lock de clock.cpp
volatile uint32_t unpinned_ready = 0;           // task-uri READY nefixate din toate cozile (indiciu pentru idle)
volatile uint64_t migrations = 0;               // task-uri luate de un CPU idle din coada altuia

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

bool queued(const Task* t){
    return t->queue.next != &t->queue;
}

void enqueue_on(uint32_t cpu, Task* t){
    RunQueue& rq = rqs[cpu];
    t->cpu = cpu;
    if(is_rt(t)){
        rt_enqueue(rq.rt, t);
    } else {
        push_back(rq.normal, t);
    }
    rq.ready++;
    if(t->affinity < 0){
        unpinned_ready++;
    }
}

void dequeue(Task* t){
    RunQueue& rq = rqs[t->cpu];
    if(is_rt(t)){
        rt_remove(rq.rt, t);
    } else {
        unlink(t);
    }
    rq.ready--;
    if(t->affinity < 0){
        unpinned_ready--;
    }
}

Task* pick(RunQueue& rq){
    Task* t = rt_pick(rq.rt);
    if(!t){
        t = pop_front(rq.normal);
    }
    if(t){
        rq.ready--;
        if(t->affinity < 0){
            unpinned_ready--;
        }
    }
    return t;
}

bool cpu_idle(uint32_t cpu){
    const PerCpu& c = cpus[cpu];
    return c.online && c.current == c.idle && !rqs[cpu].ready;
}

// CPU-ul pentru un task care devine READY: cel fixat; altfel ultimul pe care a rulat daca e liber,
// altfel primul CPU liber; daca toate sunt ocupate ramane pe ultimul
uint32_t select_cpu(const Task* t){
    if(t->affinity >= 0){
        return (uint32_t)t->affinity;
    }
    uint32_t n = smp_cpu_count();
    if(n == 1){
        return 0;
    }
    uint32_t last = t->cpu < (uint32_t)MAX_CPUS && cpus[t->cpu].online ? t->cpu : cpu_id();
    if(cpu_idle(last)){
        return last;
    }
    // CPU-urile pornite au indecsii 0..n-1 (smp_init le numeroteaza la rand)
    for(uint32_t i = 0; i < n; i++){
        if(cpu_idle(i)){
            return i;
        }
    }
    return last;
}

// t tocmai a intrat in coada lui cpu: preemptie daca CPU-ul e liber sau t e un RT mai important
void check_preempt(uint32_t cpu, const Task* t){
    PerCpu& c = cpus[cpu];
    Task* cur = c.current;
    if(cur == c.idle || (is_rt(t) && (!is_rt(cur) || t->prio > cur->prio))){
        c.need_resched = true;
        smp_send_resched(cpu);
    }
}

// Task READY -> coada potrivita. true daca ajunge pe CPU-ul curent si trebuie sa ruleze acum
bool make_ready(Task* t){
    t->state = TaskState::READY;
    uint32_t cpu = select_cpu(t);
    enqueue_on(cpu, t);
    check_preempt(cpu, t);
    return cpu == cpu_id() && this_cpu()->need_resched;
}

bool interrupts_enabled(){
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    return rflags & RFLAGS_IF;
}

void update_next_wake(){
    next_wake = empty(sleepers) ? SCHED_NO_WAKE : task_of(sleepers.next)->wake_ns;
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
        update_next_wake();
        clock_reprogram();                      // noul cel mai apropiat eveniment, pe CPU-ul curent
    }
}

// Task-urile terminate nu mai ruleaza (lock-ul a fost tinut pana dupa switch_to), deci stiva lor poate
// fi eliberata de oricine; task_destroy ia alte lock-uri, asa ca ruleaza fara sched_lock
void reap(){
    for(;;){
        Task* t;
        {
            SpinlockGuard lock(sched_lock);
            t = pop_front(zombies);
        }
        if(!t){
            return;
        }
        task_destroy(t);
    }
}

bool stealable(const Task* t){
    return t->affinity < 0;
}

// Pull la idle: cel mai important task nefixat din coada cea mai lunga a altui CPU
Task* steal(uint32_t me){
    if(!unpinned_ready){
        return nullptr;
    }
    uint32_t order[MAX_CPUS];
    uint32_t n = 0;
    uint32_t count = smp_cpu_count();
    for(uint32_t i = 0; i < count; i++){
        if(i != me && cpus[i].online && rqs[i].ready){
            uint32_t j = n++;
            while(j > 0 && rqs[order[j - 1]].ready < rqs[i].ready){
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
    }
    for(uint32_t k = 0; k < n; k++){
        RunQueue& rq = rqs[order[k]];
        Task* t = rt_pick_if(rq.rt, stealable);
        if(!t){
            for(ListLink* l = rq.normal.next; l != &rq.normal; l = l->next){
                if(stealable(task_of(l))){
                    t = task_of(l);
                    unlink(t);
                    break;
                }
            }
        }
        if(t){
            rq.ready--;
            unpinned_ready--;
            migrations++;
            return t;
        }
    }
    return nullptr;
}

// Task nefixat lasat in coada unui CPU ocupat: un CPU liber (fara tick) e trezit sa il ia
void kick_idle(uint32_t me){
    uint32_t n = smp_cpu_count();
    for(uint32_t i = 0; i < n; i++){
        if(i != me && cpu_idle(i) && !cpus[i].need_resched){
            cpus[i].need_resched = true;
            smp_send_resched(i);
            return;
        }
    }
}

uint64_t kstack_top(Task* t){
    return (uint64_t)t->kstack + TASK_KSTACK_SIZE;
}

// Dupa switch_to, pe task-ul nou: contextul celui vechi e salvat, poate fi ales de alt CPU
void finish_switch(){
    PerCpu* c = this_cpu();
    if(c->prev){
        c->prev->on_cpu = false;
        c->prev = nullptr;
    }
}

// Alege urmatorul task si comuta pe el; se apeleaza cu sched_lock luat si intreruperile oprite
void schedule(){
    PerCpu* c = this_cpu();
    c->need_resched = false;
    Task* prev = c->current;
    if(prev->state == TaskState::RUNNING){
        prev->state = TaskState::READY;
        if(prev != c->idle){
            // Affinity schimbat cat rula: pleaca in coada CPU-ului nou
            uint32_t cpu = prev->affinity >= 0 ? (uint32_t)prev->affinity : c->id;
            enqueue_on(cpu, prev);
            if(cpu != c->id){
                check_preempt(cpu, prev);
            } else if(prev->affinity < 0){
                kick_idle(c->id);
            }
        }
    }
    Task* next = pick(rqs[c->id]);
    if(!next){
        next = steal(c->id);
    }
    if(!next){
        next = c->idle;
    }
    next->state = TaskState::RUNNING;
    next->slice_left = SLICE_TICKS;
    next->cpu = c->id;
    if(next == prev){
        return;
    }
    next->on_cpu = true;
    if(next->as->pml4_phys != c->cr3){
        as_switch(*next->as);
    }
    tss_set_rsp0(kstack_top(next));
    syscall_set_kstack(kstack_top(next));
    c->current = next;
    c->prev = prev;
    if(prev == c->idle){
        clock_reprogram();                      // AP-ul nu avea tick cat a fost liber
    }
    switch_to(&prev->ctx, &next->ctx);
    // Aici se revine cand prev e ales din nou, posibil pe alt CPU
    finish_switch();
}

}

// Prima instructiune C a unui task nou (task_trampoline): termina comutarea inceputa in schedule()
extern "C" void sched_task_start(){
    finish_switch();
    sched_lock.unlock();
    __asm__ volatile("sti");
}

void sched_init(){
    for(RunQueue& rq : rqs){
        rq.normal.prev = rq.normal.next = &rq.normal;
        rt_queue_init(rq.rt);
        rq.ready = 0;
    }
    PerCpu* c = this_cpu();
    Task* idle = c->current;
    if(!idle || idle->tid != 0 || c->idle != idle){
        panic("sched_init: task 0 lipseste");
    }
    idle->slice_left = SLICE_TICKS;
    __asm__ volatile("mov %%cr3, %0" : "=r"(c->cr3));
    tss_set_rsp0(kstack_top(idle));
    kprintf("[OK] sched (RT %u-%u peste NORMAL round-robin, timeslice %u ms, runqueue per CPU)\n", RT_PRIO_MIN,
            RT_PRIO_MAX, SCHED_TIMESLICE_MS);
}

void sched_init_cpu(Task* idle){
    PerCpu* c = this_cpu();
    idle->state = TaskState::RUNNING;
    idle->on_cpu = true;
    idle->slice_left = SLICE_TICKS;
    c->idle = idle;
    c->current = idle;
    __asm__ volatile("mov %%cr3, %0" : "=r"(c->cr3));
    tss_set_rsp0(kstack_top(idle));
    syscall_set_kstack(kstack_top(idle));
}

void sched_add(Task* t){
    bool can_switch = interrupts_enabled();
    SpinlockGuard lock(sched_lock);
    bool now = make_ready(t);
    // Un task RT mai important ruleaza imediat; in IRQ sau sub lock se asteapta iesirea din IRQ
    if(is_rt(t) && now && can_switch){
        schedule();
    }
}

void yield(){
    reap();
    SpinlockGuard lock(sched_lock);
    schedule();
}

void task_exit(){
    __asm__ volatile("cli");
    Task* t = task_current();
    if(t == this_cpu()->idle){
        panic("task_exit: task-ul idle %lu nu poate iesi", t->tid);
    }
    sched_lock.lock();
    t->state = TaskState::ZOMBIE;
    push_back(zombies, t);
    schedule();
    panic("task_exit: task-ul %lu a revenit dupa ZOMBIE", t->tid);
}

bool sched_sleep_until(uint64_t wake_ns){
    Task* t = task_current();
    if(t == this_cpu()->idle || !hal_irqchip_smp()){
        return false;
    }
    SpinlockGuard lock(sched_lock);
    t->wake_ns = wake_ns;
    t->state = TaskState::BLOCKED;
    insert_sleeper(t);
    schedule();
    return true;
}

void sched_block(){
    Task* t = task_current();
    if(t == this_cpu()->idle){
        panic("sched_block: task-ul idle nu are voie sa se blocheze");
    }
    SpinlockGuard lock(sched_lock);
    // Un sched_wake venit de pe alt CPU intre BLOCKED si acest lock a anulat deja blocarea
    if(t->state == TaskState::BLOCKED){
        schedule();
    }
}

void sched_wake(Task* t){
    bool can_switch = interrupts_enabled();
    SpinlockGuard lock(sched_lock);
    if(t->state != TaskState::BLOCKED){
        return;
    }
    if(t->on_cpu){
        // Inca n-a ajuns in sched_block (ruleaza pe alt CPU): nu se mai blocheaza
        t->state = TaskState::RUNNING;
        return;
    }
    if(queued(t)){
        unlink(t);                              // adormit cu sched_sleep_until
        update_next_wake();
    }
    if(make_ready(t) && is_rt(t) && can_switch){
        schedule();
    }
}

void sched_set_class(Task* t, TaskClass cls, uint8_t prio){
    SpinlockGuard lock(sched_lock);
    bool in_rq = t->state == TaskState::READY && queued(t);
    if(in_rq){
        dequeue(t);
    }
    t->cls = cls;
    t->prio = prio;
    if(in_rq){
        uint32_t cpu = t->cpu;
        enqueue_on(cpu, t);
        check_preempt(cpu, t);
    }
}

bool sched_set_affinity(Task* t, int32_t cpu){
    if(cpu >= MAX_CPUS || (cpu >= 0 && !cpus[cpu].online) || t == cpus[t->cpu].idle){
        return false;
    }
    bool move_self = false;
    {
        SpinlockGuard lock(sched_lock);
        if(t->state == TaskState::READY && queued(t)){
            // Scos si pus la loc: contorul de task-uri nefixate depinde de affinity
            dequeue(t);
            t->affinity = cpu;
            uint32_t target = cpu >= 0 ? (uint32_t)cpu : t->cpu;
            enqueue_on(target, t);
            check_preempt(target, t);
        } else {
            t->affinity = cpu;
            if(cpu >= 0 && t->cpu != (uint32_t)cpu && t->state == TaskState::RUNNING){
                // schedule() il muta in coada CPU-ului nou
                if(t == task_current()){
                    move_self = true;
                } else {
                    cpus[t->cpu].need_resched = true;
                    smp_send_resched(t->cpu);
                }
            }
        }
    }
    if(move_self && interrupts_enabled()){
        yield();
    }
    return true;
}

void sched_timer_event(uint64_t now){
    if(next_wake > now){
        return;
    }
    SpinlockGuard lock(sched_lock);
    while(!empty(sleepers) && task_of(sleepers.next)->wake_ns <= now){
        make_ready(pop_front(sleepers));
    }
    update_next_wake();
}

uint64_t sched_next_wake(){
    return next_wake;
}

void sched_tick(uint64_t now){
    PerCpu* c = this_cpu();
    if(c->id == 0){
        watchdog_tick(now);
    }
    Task* cur = c->current;
    const RunQueue& rq = rqs[c->id];
    if(cur == c->idle){
        // Pull la idle: si cand coada proprie e goala, dar altele au task-uri READY
        if(rq.ready || unpinned_ready){
            c->need_resched = true;
        }
    } else if(is_rt(cur)){
        // RT: fara timeslice; cedeaza doar unui RT mai important sau cand se blocheaza
        if(rt_top_prio(rq.rt) > cur->prio){
            c->need_resched = true;
        }
    } else if(cur->slice_left == 0 || --cur->slice_left == 0){
        c->need_resched = true;
    }
}

void sched_irq_exit(){
    PerCpu* c = this_cpu();
    // Task periodic oprit de watchdog in timp ce rula (ex. bucla infinita): iese aici, dupa EOI
    Task* cur = c->current;
    if(cur->rt && cur->rt->stop){
        task_exit();
    }
    if(c->need_resched){
        SpinlockGuard lock(sched_lock);
        schedule();
    }
}

uint64_t sched_migrations(){
    return migrations;
}

void sched_idle(){
    __asm__ volatile("sti");
    for(;;){
        reap();
        PerCpu* c = this_cpu();
        if(rqs[c->id].ready || unpinned_ready || c->need_resched){
            SpinlockGuard lock(sched_lock);
            schedule();
        }
        // sti; hlt e atomic: un IPI venit dupa verificare trezeste CPU-ul
        __asm__ volatile("cli");
        if(!rqs[c->id].ready && !c->need_resched){
            __asm__ volatile("sti; hlt");
        } else {
            __asm__ volatile("sti");
        }
    }
}
