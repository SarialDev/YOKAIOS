#include "sync/pollwait.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "proc/signal.h"

namespace {

// Fiecare task care asteapta sta intr-un Waiter de pe stiva lui (ca in net/sys_arch.cpp): Task::queue nu se poate
// folosi, fiindca la expirarea termenului task-ul intra in coada de rulare fara sa treaca pe aici
struct Waiter {
    Waiter* prev;
    Waiter* next;
    Task* task;
};

Spinlock plock("pollwait");
Waiter head = {&head, &head, nullptr};
volatile uint64_t seq = 0;

void unlink(Waiter* w){
    if(w->next != w){
        w->prev->next = w->next;
        w->next->prev = w->prev;
        w->prev = w->next = w;
    }
}

}

uint64_t poll_seq(){
    return __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
}

void poll_notify(){
    uint64_t flags = plock.lock_irqsave();
    __atomic_add_fetch(&seq, 1, __ATOMIC_ACQ_REL);
    while(head.next != &head){
        Waiter* w = head.next;
        unlink(w);
        sched_wake(w->task);
    }
    plock.unlock_irqrestore(flags);
}

void poll_wait(uint64_t s, uint64_t deadline_ns){
    Task* t = task_current();
    uint64_t flags = plock.lock_irqsave();
    if(poll_seq() != s || signal_pending()){
        plock.unlock_irqrestore(flags);
        return;
    }
    Waiter w;
    w.task = t;
    w.prev = head.prev;
    w.next = &head;
    head.prev->next = &w;
    head.prev = &w;
    t->state = TaskState::BLOCKED;
    plock.unlock();                     // intreruperile raman oprite pana in sched_block
    if(deadline_ns){
        sched_block_until(deadline_ns);
    } else {
        sched_block();
    }
    plock.lock();
    unlink(&w);
    plock.unlock_irqrestore(flags);
}

void poll_interrupt(Task* t){
    uint64_t flags = plock.lock_irqsave();
    for(Waiter* w = head.next; w != &head; w = w->next){
        if(w->task == t){
            unlink(w);
            sched_wake(t);
            break;
        }
    }
    plock.unlock_irqrestore(flags);
}
