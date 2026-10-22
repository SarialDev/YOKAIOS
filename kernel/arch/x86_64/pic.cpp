#include "arch/x86_64/pic.h"
#include "io.h"

namespace {

const uint16_t PIC1_CMD = 0x20;
const uint16_t PIC1_DATA = 0x21;
const uint16_t PIC2_CMD = 0xA0;
const uint16_t PIC2_DATA = 0xA1;

const uint8_t ICW1_INIT = 0x11;        // init + asteapta ICW4
const uint8_t ICW4_8086 = 0x01;
const uint8_t OCW2_EOI = 0x20;
const uint8_t CASCADE_IRQ = 2;

// Port 0x80 nefolosit: mica pauza pentru PIC-uri vechi
void io_wait(){
    outb(0x80, 0);
}

uint16_t data_port(uint8_t irq){
    return irq < 8 ? PIC1_DATA : PIC2_DATA;
}

}

void pic_init(){
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DATA, PIC_IRQ_BASE); io_wait();
    outb(PIC2_DATA, PIC_IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 1 << CASCADE_IRQ); io_wait();
    outb(PIC2_DATA, CASCADE_IRQ); io_wait();
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(uint8_t irq){
    uint16_t port = data_port(irq);
    outb(port, inb(port) & ~(1 << (irq & 7)));
    if(irq >= 8){
        pic_unmask(CASCADE_IRQ);
    }
}

void pic_mask(uint8_t irq){
    uint16_t port = data_port(irq);
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_eoi(uint8_t irq){
    if(irq >= 8){
        outb(PIC2_CMD, OCW2_EOI);
    }
    outb(PIC1_CMD, OCW2_EOI);
}
