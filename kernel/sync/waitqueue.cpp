#include "sync/waitqueue.h"
#include "sched/sched.h"
#include "proc/signal.h"

namespace {

// Protejeaza Task::intr_wq: cine trimite un semnal citeste coada si o trezeste sub acest lock, deci coada nu poate
// disparea (fd inchis, pipe eliberat) intre citire si folosire
Spinlock intr_lock("intr");

Task* task_of(ListLink* l){
    return (Task*)((uint8_t*)l - __builtin_offsetof(Task, queue));
}

}

// Se apeleaza cu intreruperile oprite: intre BLOCKED si schedule() nu poate veni un wake pierdut
void WaitQueue::enqueue_current(){
    Task* t = task_current();
    lock.lock();
    ListLink& l = t->queue;
    l.prev = waiters.prev;
    l.next = &waiters;
    waiters.prev->next = &l;
    waiters.prev = &l;
    t->state = TaskState::BLOCKED;
    lock.unlock();
}

void WaitQueue::wait(){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    enqueue_current();
    sched_block();
    if(flags & (1UL << 9)){
        __asm__ volatile("sti" : : : "memory");
    }
}

void WaitQueue::wait(Spinlock& outer){
    enqueue_current();
    outer.unlock();
    sched_block();
    outer.lock();
}

int WaitQueue::wake_one(){
    uint64_t flags = lock.lock_irqsave();
    int woken = 0;
    if(!empty()){
        Task* t = task_of(waiters.next);
        ListLink& l = t->queue;
        l.prev->next = l.next;
        l.next->prev = l.prev;
        l.prev = l.next = &l;
        sched_wake(t);
        woken = 1;
    }
    lock.unlock_irqrestore(flags);
    return woken;
}

int WaitQueue::wake_all(){
    int woken = 0;
    while(wake_one()){
        woken++;
    }
    return woken;
}

bool WaitQueue::wait_intr(Spinlock& outer){
    Task* t = task_current();
    uint64_t flags = intr_lock.lock_irqsave();
    t->intr_wq = this;
    intr_lock.unlock_irqrestore(flags);
    // Semnalul se verifica sub lock-ul cozii, impreuna cu punerea in coada: cine il trimite ia apoi acelasi lock,
    // deci ori vedem semnalul aici, ori il gaseste pe t in coada si il trezeste
    bool slept = false;
    lock.lock();
    if(!signal_pending()){
        ListLink& l = t->queue;
        l.prev = waiters.prev;
        l.next = &waiters;
        waiters.prev->next = &l;
        waiters.prev = &l;
        t->state = TaskState::BLOCKED;
        slept = true;
    }
    lock.unlock();
    if(slept){
        outer.unlock();
        sched_block();
        outer.lock();
    }
    flags = intr_lock.lock_irqsave();
    t->intr_wq = nullptr;
    intr_lock.unlock_irqrestore(flags);
    return !signal_pending();
}

void WaitQueue::interrupt(Task* t){
    uint64_t flags = lock.lock_irqsave();
    // Task::queue serveste si pentru coada de rulare: dupa un wake normal t e deja acolo, deci il cautam in aceasta coada
    bool here = false;
    for(ListLink* w = waiters.next; w != &waiters; w = w->next){
        if(w == &t->queue){
            here = true;
            break;
        }
    }
    ListLink& l = t->queue;
    if(here){
        l.prev->next = l.next;
        l.next->prev = l.prev;
        l.prev = l.next = &l;
        sched_wake(t);
    }
    lock.unlock_irqrestore(flags);
}

void WaitQueue::interrupt_task(Task* t){
    uint64_t flags = intr_lock.lock_irqsave();
    WaitQueue* q = t->intr_wq;
    if(q){
        q->interrupt(t);
    }
    intr_lock.unlock_irqrestore(flags);
}
