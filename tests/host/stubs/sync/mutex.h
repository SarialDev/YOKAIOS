#pragma once
// Stub pentru testele pe gazda: un singur thread, fara blocare
class Mutex {
public:
    constexpr Mutex(const char* = "mutex"){}
    void lock(){}
    bool try_lock(){ return true; }
    void unlock(){}
};
class MutexGuard {
public:
    explicit MutexGuard(Mutex&){}
};
