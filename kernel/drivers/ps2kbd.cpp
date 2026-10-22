#include "drivers/ps2kbd.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/ioapic.h"
#include "interrupt.h"
#include "io.h"

namespace {

const uint16_t KBD_DATA = 0x60;
const uint16_t KBD_STATUS = 0x64;
const uint8_t STATUS_OUTPUT_FULL = 0x01;

KeyCallback on_key = nullptr;

void kbd_irq(InterruptFrame*){
    uint8_t scancode = inb(KBD_DATA);
    if(on_key){
        on_key(scancode);
    }
}

}

void ps2kbd_init(KeyCallback cb){
    on_key = cb;
    // Goleste bytes ramasi de la BIOS/GRUB ca IRQ1 sa poata porni
    while(inb(KBD_STATUS) & STATUS_OUTPUT_FULL){
        inb(KBD_DATA);
    }
    irq_register(PIC_IRQ_BASE + KBD_IRQ, kbd_irq);
    // Prin IOAPIC cand exista; PIC doar ca rezerva pentru masini fara APIC
    if(!lapic_enabled() || !ioapic_route(KBD_IRQ, PIC_IRQ_BASE + KBD_IRQ, lapic_id())){
        pic_unmask(KBD_IRQ);
    }
}
