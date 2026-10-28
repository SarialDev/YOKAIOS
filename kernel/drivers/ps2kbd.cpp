#include "drivers/ps2kbd.h"
#include "arch/x86_64/pic.h"
#include "hal/irqchip.h"
#include "hal/input.h"
#include "time/clock.h"
#include "interrupt.h"
#include "io.h"

namespace {

const uint16_t KBD_DATA = 0x60;
const uint16_t KBD_STATUS = 0x64;
const uint8_t STATUS_OUTPUT_FULL = 0x01;

class Ps2Keyboard final : public InputDevice {
public:
    constexpr Ps2Keyboard() = default;
    InputKind kind() const override { return InputKind::Keyboard; }
};

Ps2Keyboard keyboard;

void kbd_irq(InterruptFrame*){
    uint8_t scancode = inb(KBD_DATA);
    InputEvent ev;
    ev.type = InputType::Key;
    ev.raw = scancode;
    ev.code = scancode & 0x7F;
    ev.value = (scancode & 0x80) ? 0 : 1;
    ev.time_ns = monotonic_ns();
    input_push(ev);
}

}

void ps2kbd_init(){
    hal_register(HalKind::Input, &keyboard, "ps2-kbd");
    // Goleste bytes ramasi de la BIOS/GRUB ca IRQ1 sa poata porni
    while(inb(KBD_STATUS) & STATUS_OUTPUT_FULL){
        inb(KBD_DATA);
    }
    irq_register(PIC_IRQ_BASE + KBD_IRQ, kbd_irq);
    InterruptController* chip = hal_irqchip();
    chip->route_irq(KBD_IRQ, PIC_IRQ_BASE + KBD_IRQ, chip->cpu_id());
}
