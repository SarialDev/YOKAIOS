#include "time/tsc.h"
#include "hal/timer.h"

namespace {

void cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d){
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0));
}

class TscClock final : public ClockSource {
public:
    constexpr TscClock() = default;
    uint64_t read() override { return rdtsc(); }
    uint32_t flags() const override { return fl; }
    void set_flags(uint32_t f){ fl = f; }
private:
    uint32_t fl = 0;
};

TscClock tsc_clock;

}

bool tsc_invariant(){
    uint32_t a, b, c, d;
    cpuid(0x80000000, a, b, c, d);
    if(a < 0x80000007){
        return false;
    }
    cpuid(0x80000007, a, b, c, d);
    return d & (1 << 8);
}

bool cpu_hypervisor(){
    uint32_t a, b, c, d;
    cpuid(1, a, b, c, d);
    return c & (1u << 31);
}

void tsc_register(){
    tsc_clock.set_flags((tsc_invariant() ? CLOCK_INVARIANT : 0) | (cpu_hypervisor() ? CLOCK_VIRTUAL : 0));
    hal_register(HalKind::Clock, &tsc_clock, "tsc");
}
