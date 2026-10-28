#pragma once
#include "types.h"
#include "sched/task.h"
#include "sync/spinlock.h"

// Coada de task-uri blocate in asteptarea unui eveniment
class WaitQueue {
public:
    constexpr WaitQueue() : waiters{&waiters, &waiters}, lock("waitqueue"){}
    // Blocheaza task-ul curent pana la wake. Cu "outer": il elibereaza atomic fata de wake si il reia la trezire
    // (outer trebuie luat cu lock_irqsave, ca intreruperile sa fie oprite)
    void wait();
    void wait(Spinlock& outer);
    // Trezeste primul task (FIFO) / toate; intorc cate au fost trezite
    int wake_one();
    int wake_all();
    bool empty() const { return waiters.next == &waiters; }
    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;
private:
    void enqueue_current();
    ListLink waiters;
    Spinlock lock;
};
