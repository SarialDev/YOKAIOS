#include "sync/mutex.h"
#include "core/panic.h"

void Mutex::lock(){
    Task* self = task_current();
    uint64_t flags = guard.lock_irqsave();
    if(owner == self){
        panic("mutex %s: re-lock de catre task-ul %lu (fara recursivitate)", name, self->tid);
    }
    while(owner){
        if(self->cls == TaskClass::RT && self->prio > owner->prio){
            if(!boosted){
                owner_prio = owner->prio;
                boosted = true;
            }
            owner->prio = self->prio;
        }
        waiters.wait(guard);
    }
    owner = self;
    guard.unlock_irqrestore(flags);
}

bool Mutex::try_lock(){
    uint64_t flags = guard.lock_irqsave();
    bool ok = !owner;
    if(ok){
        owner = task_current();
    }
    guard.unlock_irqrestore(flags);
    return ok;
}

void Mutex::unlock(){
    uint64_t flags = guard.lock_irqsave();
    Task* self = task_current();
    if(owner != self){
        panic("mutex %s: unlock de catre task-ul %lu, owner %lu", name, self->tid, owner ? owner->tid : 0);
    }
    if(boosted){
        self->prio = owner_prio;
        boosted = false;
    }
    owner = nullptr;
    waiters.wake_one();
    guard.unlock_irqrestore(flags);
}
