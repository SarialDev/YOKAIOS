#include "time/clock.h"
#include "time/hpet.h"
#include "time/pit.h"
#include "time/tsc.h"
#include "arch/x86_64/lapic.h"
#include "interrupt.h"
#include "lib/kprintf.h"
#include "sched/sched.h"

volatile uint64_t jiffies = 0;

namespace {

const uint32_t CALIBRATE_MS = 50;

uint64_t tsc_hz = 0;
uint64_t tsc_base = 0;
uint64_t ns_mult = 0;           // ns = (delta_tsc * ns_mult) >> 32
bool use_tsc = false;

void timer_tick(InterruptFrame*){
    jiffies++;
    sched_tick();
}

}

void clock_init(){
    bool hpet = hpet_init();
    void (*wait_ms)(uint32_t) = hpet ? hpet_wait_ms : pit_wait_ms;

    bool invariant = tsc_invariant();
    bool hypervisor = cpu_hypervisor();
    tsc_hz = tsc_calibrate(wait_ms, CALIBRATE_MS);
    ns_mult = (1000000000UL << 32) / tsc_hz;
    tsc_base = rdtsc();
    // Pe hardware fara TSC invariant frecventa poate varia; atunci timpul vine din jiffies
    use_tsc = invariant || hypervisor;

    if(lapic_enabled()){
        uint32_t ticks = lapic_timer_measure(wait_ms, CALIBRATE_MS);
        irq_register(TIMER_VECTOR, timer_tick);
        lapic_timer_periodic(ticks / CALIBRATE_MS * 1000 / TIMER_HZ, TIMER_VECTOR);
        kprintf("LAPIC timer: %u tick/ms, %u Hz\n", ticks / CALIBRATE_MS, TIMER_HZ);
    }
    kprintf("Ceas: referinta %s, TSC %s\n", hpet ? "HPET" : "PIT",
            invariant ? "invariant" : hypervisor ? "virtual (hypervisor)" : "neinvariant (folosesc jiffies)");
    kprintf("[OK] timer TSC=%lu MHz\n", tsc_hz / 1000000);
}

uint64_t monotonic_ns(){
    if(use_tsc){
        uint64_t delta = rdtsc() - tsc_base;
        return (uint64_t)(((unsigned __int128)delta * ns_mult) >> 32);
    }
    return jiffies * (1000000000UL / TIMER_HZ);
}

uint64_t uptime_ms(){
    return monotonic_ns() / 1000000;
}

void sleep_ms(uint32_t ms){
    uint64_t end = monotonic_ns() + (uint64_t)ms * 1000000;
    if(sched_sleep_until(end)){
        return;
    }
    // Task 0 (idle) sau fara tick: asteptare cu hlt
    while(monotonic_ns() < end){
        __asm__ volatile("hlt");
    }
}
