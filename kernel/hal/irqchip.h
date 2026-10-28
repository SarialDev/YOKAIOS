#pragma once
#include "hal/hal.h"

const uint32_t IRQCHIP_SMP = 1;         // IPI, INIT/SIPI pentru AP, NMI catre celelalte CPU-uri
const uint32_t IRQCHIP_TIMER = 1 << 1;  // timer local per CPU (LAPIC)

// Controler de intreruperi: PIC 8259 (fallback) sau LAPIC+IOAPIC. Cel mai recent inregistrat e activ
class InterruptController : public HalDevice {
public:
    virtual uint32_t caps() const = 0;
    // Termina intreruperea cu vectorul dat (apelat din dispatcher dupa handler)
    virtual void eoi(uint8_t vector) = 0;
    // Directioneaza un IRQ ISA catre vector pe CPU-ul `cpu` (id hardware); false daca nu se poate
    virtual bool route_irq(uint8_t irq, uint8_t vector, uint32_t cpu) = 0;
    // Id-ul hardware al CPU-ului curent
    virtual uint32_t cpu_id() = 0;

    // IRQCHIP_SMP
    virtual void init_ap() {}
    virtual void send_ipi(uint32_t, uint8_t) {}
    virtual void send_init(uint32_t) {}
    virtual void send_startup(uint32_t, uint8_t) {}
    virtual void nmi_others() {}

    // IRQCHIP_TIMER: timer local, diviziune fixa; tick-urile se numara in unitatile lui
    virtual uint32_t timer_measure(void (*)(uint32_t), uint32_t) { return 0; }
    virtual void timer_periodic(uint32_t, uint8_t) {}
    virtual void timer_oneshot(uint8_t) {}
    virtual void timer_arm(uint32_t) {}
    virtual void timer_stop() {}

    bool has(uint32_t cap) const { return caps() & cap; }
protected:
    constexpr InterruptController() = default;
};

inline InterruptController* hal_irqchip(){
    return static_cast<InterruptController*>(hal_primary_fast(HalKind::IrqChip));
}

// true cand controlerul activ are LAPIC (inlocuieste fostul lapic_enabled)
inline bool hal_irqchip_smp(){
    InterruptController* c = hal_irqchip();
    return c && c->has(IRQCHIP_SMP);
}
