#include "time/clock.h"
#include "time/hpet.h"
#include "time/pit.h"
#include "hal/timer.h"
#include "hal/irqchip.h"
#include "interrupt.h"
#include "lib/kprintf.h"
#include "sched/sched.h"
#include "arch/x86_64/percpu.h"

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

// Urmatorul eveniment al CPU-ului curent: tick-ul lui de 1 ms sau prima trezire (eliberare RT, sleep),
// care vine mai repede. Trezirile sunt comune, deci le prinde primul CPU care ajunge la ele.
// Un AP liber nu are tick (il trezeste un IPI cand primeste de lucru); BSP-ul ticaie mereu (jiffies, watchdog)
void program_event(){
    PerCpu* c = this_cpu();
    bool tickless = c->id != 0 && c->current == c->idle && !c->need_resched;
    uint64_t target = tickless ? SCHED_NO_WAKE : c->next_tick_ns;
    uint64_t wake = sched_next_wake();
    if(wake < target){
        target = wake;
    }
    if(target == SCHED_NO_WAKE){
        hal_irqchip()->timer_stop();
        return;
    }
    uint64_t now = monotonic_ns();
    uint64_t delta = target > now ? target - now : 0;
    uint64_t ticks = delta * lapic_ticks_per_ms / 1000000;
    hal_irqchip()->timer_arm(ticks > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)ticks);
}

// Pe fiecare CPU; jiffies numara doar tick-urile BSP-ului
void timer_tick(InterruptFrame*){
    PerCpu* c = this_cpu();
    if(!oneshot){
        if(c->id == 0){
            jiffies++;
        }
        sched_timer_event(monotonic_ns());
        sched_tick(monotonic_ns());
        return;
    }
    uint64_t now = monotonic_ns();
    sched_timer_event(now);
    if(now >= c->next_tick_ns){
        uint64_t behind = (now - c->next_tick_ns) / TICK_NS + 1;
        if(behind > MAX_CATCH_UP_TICKS){
            c->next_tick_ns = now + TICK_NS;
        } else {
            c->next_tick_ns += behind * TICK_NS;
        }
        if(c->id == 0){
            jiffies += behind;
        }
        sched_tick(now);
    }
    program_event();
}

void start_local_timer(){
    if(oneshot){
        this_cpu()->next_tick_ns = monotonic_ns() + TICK_NS;
        hal_irqchip()->timer_oneshot(TIMER_VECTOR);
        program_event();
    } else {
        hal_irqchip()->timer_periodic((uint32_t)(lapic_ticks_per_ms * 1000 / TIMER_HZ), TIMER_VECTOR);
    }
}

}

void clock_init(){
    bool hpet = hpet_init();
    void (*wait_ms)(uint32_t) = hpet ? hpet_wait_ms : pit_wait_ms;

    ClockSource* clk = hal_clock();
    bool invariant = clk->flags() & CLOCK_INVARIANT;
    bool hypervisor = clk->flags() & CLOCK_VIRTUAL;
    tsc_hz = clk->calibrate(wait_ms, CALIBRATE_MS);
    ns_mult = (1000000000UL << 32) / tsc_hz;
    tsc_base = clk->read();
    // Pe hardware fara TSC invariant frecventa poate varia; atunci timpul vine din jiffies
    use_tsc = invariant || hypervisor;

    if(hal_irqchip_smp()){
        uint32_t ticks = hal_irqchip()->timer_measure(wait_ms, CALIBRATE_MS);
        lapic_ticks_per_ms = ticks / CALIBRATE_MS;
        irq_register(TIMER_VECTOR, timer_tick);
        // One-shot are nevoie de un ceas care nu depinde de tick (TSC); altfel ramane periodic
        oneshot = use_tsc;
        start_local_timer();
        kprintf("LAPIC timer: %lu tick/ms, %u Hz, %s\n", lapic_ticks_per_ms, TIMER_HZ,
                oneshot ? "one-shot (evenimente RT de inalta rezolutie)" : "periodic");
    }
    kprintf("Ceas: referinta %s, TSC %s\n", hpet ? "HPET" : "PIT",
            invariant ? "invariant" : hypervisor ? "virtual (hypervisor)" : "neinvariant (folosesc jiffies)");
    kprintf("[OK] timer TSC=%lu MHz\n", tsc_hz / 1000000);
}

void clock_init_ap(){
    // Toate LAPIC-urile merg la aceeasi frecventa: calibrarea BSP-ului e valabila
    if(lapic_ticks_per_ms){
        start_local_timer();
    }
}

uint64_t monotonic_ns(){
    if(use_tsc){
        uint64_t delta = hal_clock()->read() - tsc_base;
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

uint64_t clock_tsc_hz(){
    return tsc_hz;
}
