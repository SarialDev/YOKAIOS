#include "sync/mutex.h"
#include "core/panic.h"
#include "sched/sched.h"

void Mutex::lock(){
    Task* self = task_current();
    uint64_t flags = guard.lock_irqsave();
    if(owner == self){
        panic("mutex %s: re-lock de catre task-ul %lu (fara recursivitate)", name, self->tid);
    }
    while(owner){
        // Owner-ul ruleaza ca RT cu prioritatea celui care asteapta, ca sa elibereze repede mutex-ul
        if(self->cls == TaskClass::RT && (owner->cls != TaskClass::RT || self->prio > owner->prio)){
            if(!boosted){
                owner_prio = owner->prio;
                owner_cls = owner->cls;
                boosted = true;
            }
            sched_set_class(owner, TaskClass::RT, self->prio);
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
        sched_set_class(self, owner_cls, owner_prio);
        boosted = false;
    }
    owner = nullptr;
    waiters.wake_one();
    guard.unlock_irqrestore(flags);
}
