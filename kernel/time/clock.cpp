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

const uint64_t TICK_NS = 1000000000UL / TIMER_HZ;
const uint64_t MAX_CATCH_UP_TICKS = 100;

uint64_t tsc_hz = 0;
uint64_t tsc_base = 0;
uint64_t ns_mult = 0;           // ns = (delta_tsc * ns_mult) >> 32
bool use_tsc = false;
bool oneshot = false;           // LAPIC one-shot: tick-ul de 1 ms si trezirile sunt evenimente programate
uint64_t lapic_ticks_per_ms = 0;
uint64_t next_tick_ns = 0;

// Urmatorul eveniment: tick-ul de 1 ms sau prima trezire (eliberare RT, sleep), care vine mai repede
void program_event(){
    uint64_t target = next_tick_ns;
    uint64_t wake = sched_next_wake();
    if(wake < target){
        target = wake;
    }
    uint64_t now = monotonic_ns();
    uint64_t delta = target > now ? target - now : 0;
    uint64_t ticks = delta * lapic_ticks_per_ms / 1000000;
    lapic_timer_arm(ticks > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)ticks);
}

void timer_tick(InterruptFrame*){
    if(!oneshot){
        jiffies++;
        sched_timer_event(monotonic_ns());
        sched_tick(monotonic_ns());
        return;
    }
    uint64_t now = monotonic_ns();
    sched_timer_event(now);
    if(now >= next_tick_ns){
        uint64_t behind = (now - next_tick_ns) / TICK_NS + 1;
        if(behind > MAX_CATCH_UP_TICKS){
            next_tick_ns = now + TICK_NS;
        } else {
            next_tick_ns += behind * TICK_NS;
        }
        jiffies += behind;
        sched_tick(now);
    }
    program_event();
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
        lapic_ticks_per_ms = ticks / CALIBRATE_MS;
        irq_register(TIMER_VECTOR, timer_tick);
        // One-shot are nevoie de un ceas care nu depinde de tick (TSC); altfel ramane periodic
        oneshot = use_tsc;
        if(oneshot){
            next_tick_ns = monotonic_ns() + TICK_NS;
            lapic_timer_oneshot(TIMER_VECTOR);
            program_event();
        } else {
            lapic_timer_periodic(ticks / CALIBRATE_MS * 1000 / TIMER_HZ, TIMER_VECTOR);
        }
        kprintf("LAPIC timer: %lu tick/ms, %u Hz, %s\n", lapic_ticks_per_ms, TIMER_HZ,
                oneshot ? "one-shot (evenimente RT de inalta rezolutie)" : "periodic");
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

void clock_reprogram(){
    if(oneshot){
        program_event();
    }
}

bool clock_high_res(){
    return oneshot;
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
