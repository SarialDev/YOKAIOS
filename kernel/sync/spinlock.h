#pragma once
#include "types.h"
#include "sync/atomic.h"

// Spinlock cu __atomic. Debug: retine CPU-ul si adresa care l-au luat; re-lock pe acelasi CPU -> panic
class Spinlock {
public:
    constexpr Spinlock(const char* lock_name = "spinlock") : locked(0), owner_cpu(0), owner_pc(0), name(lock_name){}
    void lock();
    void unlock();
    bool try_lock();
    // Dezactiveaza intreruperile, apoi ia lock-ul; intoarce RFLAGS-ul anterior pentru unlock_irqrestore
    uint64_t lock_irqsave();
    void unlock_irqrestore(uint64_t flags);
    bool held() const { return locked.load() != 0; }
    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;
private:
    void acquire(uint64_t pc);
    Atomic<uint32_t> locked;
    uint32_t owner_cpu;
    uint64_t owner_pc;
    const char* name;
};

// RAII: lock_irqsave in constructor, unlock_irqrestore in destructor
class SpinlockGuard {
public:
    explicit SpinlockGuard(Spinlock& l) : lk(l), flags(l.lock_irqsave()){}
    ~SpinlockGuard(){ lk.unlock_irqrestore(flags); }
    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;
private:
    Spinlock& lk;
    uint64_t flags;
};
