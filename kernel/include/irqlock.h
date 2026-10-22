#pragma once
#include "types.h"

// Lock simplu pe un singur CPU (pana la T4.3): dezactiveaza intreruperile
// si le reactiveaza la iesire doar daca erau active
class IrqLock {
public:
    IrqLock(){
        uint64_t rflags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
        was_enabled = rflags & (1 << 9);
    }
    ~IrqLock(){
        if(was_enabled){
            __asm__ volatile("sti" : : : "memory");
        }
    }
    IrqLock(const IrqLock&) = delete;
    IrqLock& operator=(const IrqLock&) = delete;
private:
    bool was_enabled;
};
