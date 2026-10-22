#include "drivers/serial.h"
#include "io.h"

static const uint16_t COM1 = 0x3F8;
static bool serial_ok = false;

bool serial_init(){
    outb(COM1 + 1, 0x00);   // fara IRQ
    outb(COM1 + 3, 0x80);   // DLAB
    outb(COM1 + 0, 0x01);   // divizor 1 = 115200
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   // 8N1
    outb(COM1 + 2, 0xC7);   // FIFO activat, curatat, prag 14
    outb(COM1 + 4, 0x1E);   // loopback
    outb(COM1 + 0, 0xAE);
    if(inb(COM1 + 0) != 0xAE){
        return false;
    }
    outb(COM1 + 4, 0x0F);   // mod normal
    serial_ok = true;
    return true;
}

void serial_putc(char c){
    if(!serial_ok){
        return;
    }
    if(c == '\n'){
        serial_putc('\r');
    }
    while((inb(COM1 + 5) & 0x20) == 0){
    }
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char* s){
    while(*s){
        serial_putc(*s++);
    }
}
