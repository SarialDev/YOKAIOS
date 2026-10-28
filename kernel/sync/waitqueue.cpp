#include "sync/waitqueue.h"
#include "sched/sched.h"

namespace {

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
