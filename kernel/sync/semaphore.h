#pragma once
#include "types.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"

// Semafor numarabil: down() blocheaza cat timp contorul e 0
class Semaphore {
public:
    constexpr Semaphore(uint32_t initial = 0) : guard("semaphore"), waiters(), count(initial){}
    void down();
    bool try_down();
    void up();
    uint32_t value() const { return count; }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
private:
    Spinlock guard;
    WaitQueue waiters;
    uint32_t count;
};

// Eveniment unic: wait() blocheaza pana la complete(), dupa care trece imediat
class Completion {
public:
    constexpr Completion() : guard("completion"), waiters(), done(false){}
    void wait();
    void complete();
    void reset();
    bool is_done() const { return done; }
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
private:
    Spinlock guard;
    WaitQueue waiters;
    bool done;
};
