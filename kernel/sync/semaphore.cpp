#include "sync/semaphore.h"

void Semaphore::down(){
    uint64_t flags = guard.lock_irqsave();
    while(count == 0){
        waiters.wait(guard);
    }
    count--;
    guard.unlock_irqrestore(flags);
}

bool Semaphore::try_down(){
    uint64_t flags = guard.lock_irqsave();
    bool ok = count > 0;
    if(ok){
        count--;
    }
    guard.unlock_irqrestore(flags);
    return ok;
}

void Semaphore::up(){
    uint64_t flags = guard.lock_irqsave();
    count++;
    waiters.wake_one();
    guard.unlock_irqrestore(flags);
}

void Completion::wait(){
    uint64_t flags = guard.lock_irqsave();
    while(!done){
        waiters.wait(guard);
    }
    guard.unlock_irqrestore(flags);
}

void Completion::complete(){
    uint64_t flags = guard.lock_irqsave();
    done = true;
    waiters.wake_all();
    guard.unlock_irqrestore(flags);
}

void Completion::reset(){
    uint64_t flags = guard.lock_irqsave();
    done = false;
    guard.unlock_irqrestore(flags);
}
