#pragma once
#include "types.h"

// Operatii atomice peste __atomic (seq_cst implicit); T e un intreg sau un pointer
template<typename T>
class Atomic {
public:
    constexpr Atomic(T initial = T()) : value(initial){}
    T load(int order = __ATOMIC_SEQ_CST) const { return __atomic_load_n(&value, order); }
    void store(T v, int order = __ATOMIC_SEQ_CST){ __atomic_store_n(&value, v, order); }
    T exchange(T v){ return __atomic_exchange_n(&value, v, __ATOMIC_SEQ_CST); }
    T fetch_add(T v){ return __atomic_fetch_add(&value, v, __ATOMIC_SEQ_CST); }
    T fetch_sub(T v){ return __atomic_fetch_sub(&value, v, __ATOMIC_SEQ_CST); }
    // Daca valoarea e expected o inlocuieste cu desired; altfel pune in expected valoarea gasita
    bool compare_exchange(T& expected, T desired){
        return __atomic_compare_exchange_n(&value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;
private:
    T value;
};

// Indicatie pentru CPU in buclele de asteptare activa
inline void cpu_relax(){
    __asm__ volatile("pause" : : : "memory");
}

// Ce CPU ruleaza codul curent (id-ul LAPIC, 0 inainte de APIC)
uint32_t cpu_id();
