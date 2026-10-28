#include "sync/spinlock.h"
#include "core/panic.h"
#include "arch/x86_64/smp.h"

namespace {

const uint64_t RFLAGS_IF = 1UL << 9;
const uint32_t NO_OWNER = ~0u;

uint64_t save_and_cli(){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

}

void Spinlock::lock(){
    acquire((uint64_t)__builtin_return_address(0));
}

void Spinlock::acquire(uint64_t pc){
    uint32_t cpu = cpu_id();
    // owner_cpu se sterge inainte de eliberare, deci pe alt CPU nu poate parea egal cu cpu
    if(locked.load(__ATOMIC_RELAXED) && __atomic_load_n(&owner_cpu, __ATOMIC_RELAXED) == cpu){
        panic("spinlock %s: re-lock pe CPU %u (luat la %lx, din nou la %lx)", name, cpu, owner_pc, pc);
    }
    for(;;){
        uint32_t expected = 0;
        if(locked.compare_exchange(expected, 1)){
            break;
        }
        while(locked.load(__ATOMIC_RELAXED)){
            // Cu intreruperile oprite: un TLB shootdown cerut intre timp se rezolva aici, altfel deadlock
            smp_spin_poll();
            cpu_relax();
        }
    }
    __atomic_store_n(&owner_cpu, cpu, __ATOMIC_RELAXED);
    owner_pc = pc;
}

bool Spinlock::try_lock(){
    uint32_t expected = 0;
    if(!locked.compare_exchange(expected, 1)){
        return false;
    }
    __atomic_store_n(&owner_cpu, cpu_id(), __ATOMIC_RELAXED);
    owner_pc = (uint64_t)__builtin_return_address(0);
    return true;
}

void Spinlock::unlock(){
    if(!locked.load(__ATOMIC_RELAXED)){
        panic("spinlock %s: unlock fara lock", name);
    }
    owner_pc = 0;
    __atomic_store_n(&owner_cpu, NO_OWNER, __ATOMIC_RELAXED);
    locked.store(0, __ATOMIC_RELEASE);
}

uint64_t Spinlock::lock_irqsave(){
    uint64_t flags = save_and_cli();
    acquire((uint64_t)__builtin_return_address(0));
    return flags;
}

void Spinlock::unlock_irqrestore(uint64_t flags){
    unlock();
    if(flags & RFLAGS_IF){
        __asm__ volatile("sti" : : : "memory");
    }
}
