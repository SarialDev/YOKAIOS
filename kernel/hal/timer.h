#pragma once
#include "hal/hal.h"

const uint32_t CLOCK_INVARIANT = 1;     // frecventa constanta (TSC invariant)
const uint32_t CLOCK_VIRTUAL = 2;       // ceas virtual dintr-un hypervisor, rata constanta

// Contor monoton cu frecventa cunoscuta
class ClockSource : public HalDevice {
public:
    virtual uint64_t read() = 0;
    virtual uint32_t flags() const = 0;
    uint64_t freq_hz() const { return freq; }
    // Masoara frecventa contorului pe ms milisecunde, cu functia de asteptare data; intoarce Hz
    virtual uint64_t calibrate(void (*wait_ms)(uint32_t), uint32_t ms){
        uint64_t start = read();
        wait_ms(ms);
        freq = (read() - start) * 1000 / ms;
        return freq;
    }
protected:
    constexpr ClockSource() = default;
    uint64_t freq = 0;
};

inline ClockSource* hal_clock(){
    return static_cast<ClockSource*>(hal_primary_fast(HalKind::Clock));
}
