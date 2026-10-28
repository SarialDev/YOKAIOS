#pragma once
#include "sched/task.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"

// Mutex care blocheaza (nu se invarte). Fara recursivitate: re-lock de catre owner -> panic.
// Priority inheritance: un task RT care asteapta ridica prioritatea owner-ului pana la unlock
class Mutex {
public:
    constexpr Mutex(const char* mutex_name = "mutex")
        : guard("mutex"), waiters(), owner(nullptr), owner_prio(0), owner_cls(TaskClass::NORMAL), boosted(false),
          name(mutex_name){}
    void lock();
    bool try_lock();
    void unlock();
    Task* holder() const { return owner; }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
private:
    Spinlock guard;
    WaitQueue waiters;
    Task* owner;
    uint8_t owner_prio;         // clasa si prioritatea owner-ului inainte de boost
    TaskClass owner_cls;
    bool boosted;
    const char* name;
};

class MutexGuard {
public:
    explicit MutexGuard(Mutex& m) : mutex(m){ mutex.lock(); }
    ~MutexGuard(){ mutex.unlock(); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
private:
    Mutex& mutex;
};
