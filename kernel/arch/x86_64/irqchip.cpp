#include "arch/x86_64/irqchip.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/ioapic.h"
#include "hal/irqchip.h"

namespace {

class PicController final : public InterruptController {
public:
    constexpr PicController() = default;
    uint32_t caps() const override { return 0; }
    void eoi(uint8_t vector) override {
        if(vector >= PIC_IRQ_BASE && vector < PIC_IRQ_BASE + PIC_IRQ_COUNT){
            pic_eoi(vector - PIC_IRQ_BASE);
        }
    }
    bool route_irq(uint8_t irq, uint8_t, uint32_t) override {
        // Vectorul e fix (PIC_IRQ_BASE + irq)
        pic_unmask(irq);
        return true;
    }
    uint32_t cpu_id() override { return 0; }
};

class ApicController final : public InterruptController {
public:
    constexpr ApicController() = default;
    uint32_t caps() const override { return IRQCHIP_SMP | IRQCHIP_TIMER; }
    void eoi(uint8_t) override { lapic_eoi(); }
    bool route_irq(uint8_t irq, uint8_t vector, uint32_t cpu) override {
        // PIC doar ca rezerva daca IOAPIC-ul nu stie IRQ-ul
        if(!ioapic_route(irq, vector, (uint8_t)cpu)){
            pic_unmask(irq);
        }
        return true;
    }
    uint32_t cpu_id() override { return lapic_id(); }
    void init_ap() override { lapic_init_ap(); }
    void send_ipi(uint32_t dest, uint8_t vector) override { lapic_send_ipi(dest, vector); }
    void send_init(uint32_t dest) override { lapic_send_init(dest); }
    void send_startup(uint32_t dest, uint8_t page) override { lapic_send_startup(dest, page); }
    void nmi_others() override { lapic_send_nmi_others(); }
    uint32_t timer_measure(void (*wait_ms)(uint32_t), uint32_t ms) override { return lapic_timer_measure(wait_ms, ms); }
    void timer_periodic(uint32_t ticks, uint8_t vector) override { lapic_timer_periodic(ticks, vector); }
    void timer_oneshot(uint8_t vector) override { lapic_timer_oneshot(vector); }
    void timer_arm(uint32_t ticks) override { lapic_timer_arm(ticks); }
    void timer_stop() override { lapic_timer_stop(); }
};

PicController pic_controller;
ApicController apic_controller;

}

void irqchip_pic_register(){
    hal_register(HalKind::IrqChip, &pic_controller, "pic");
}

void irqchip_apic_register(){
    hal_register(HalKind::IrqChip, &apic_controller, "apic");
}
