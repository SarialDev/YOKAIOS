#include "time/pit.h"
#include "io.h"

namespace {

const uint32_t PIT_HZ = 1193182;
const uint16_t PIT_CH2 = 0x42;
const uint16_t PIT_CMD = 0x43;
const uint16_t PORT_B = 0x61;
const uint8_t PORT_B_GATE2 = 0x01;
const uint8_t PORT_B_SPEAKER = 0x02;
const uint8_t PORT_B_OUT2 = 0x20;
const uint8_t CMD_CH2_MODE0 = 0xB0;     // canal 2, lo/hi byte, mode 0 (OUT2 sus la final)
const uint32_t MAX_CHUNK_MS = 50;       // 50 ms = 59659 tick-uri, incape in 16 biti

void wait_chunk(uint32_t ms){
    uint32_t count = PIT_HZ * ms / 1000;
    uint8_t b = (inb(PORT_B) & ~PORT_B_SPEAKER) & ~PORT_B_GATE2;
    outb(PORT_B, b);
    outb(PIT_CMD, CMD_CH2_MODE0);
    outb(PIT_CH2, count & 0xFF);
    outb(PIT_CH2, count >> 8);
    outb(PORT_B, b | PORT_B_GATE2);
    while(!(inb(PORT_B) & PORT_B_OUT2)){
    }
    outb(PORT_B, b);
}

}

void pit_wait_ms(uint32_t ms){
    while(ms){
        uint32_t chunk = ms < MAX_CHUNK_MS ? ms : MAX_CHUNK_MS;
        wait_chunk(chunk);
        ms -= chunk;
    }
}
