#include "time/hpet.h"
#include "acpi/acpi.h"
#include "arch/x86_64/mmio.h"

namespace {

struct __attribute__((packed)) HpetTable {
    AcpiHeader header;
    uint32_t event_timer_block_id;
    uint8_t space_id;           // Generic Address Structure: 0 = memorie
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
};

const uint32_t REG_CAPS = 0x00;
const uint32_t REG_CONFIG = 0x10;
const uint32_t REG_COUNTER = 0xF0;
const uint64_t CAPS_COUNTER_64 = 1 << 13;
const uint64_t CONFIG_ENABLE = 1;
const uint64_t FS_PER_MS = 1000000000000UL;

volatile uint64_t* regs = nullptr;
uint64_t period_fs = 0;
uint64_t counter_mask = 0;

uint64_t read(uint32_t reg){
    return regs[reg / 8];
}

}

bool hpet_init(){
    const HpetTable* t = (const HpetTable*)acpi_find_table("HPET");
    if(!t || t->space_id != 0){
        return false;
    }
    regs = (volatile uint64_t*)mmio_map(t->address, 0x400);
    uint64_t caps = read(REG_CAPS);
    period_fs = caps >> 32;
    counter_mask = (caps & CAPS_COUNTER_64) ? ~0UL : 0xFFFFFFFFUL;
    regs[REG_CONFIG / 8] = read(REG_CONFIG) | CONFIG_ENABLE;
    return period_fs != 0;
}

bool hpet_present(){
    return period_fs != 0;
}

void hpet_wait_ms(uint32_t ms){
    uint64_t ticks = ms * FS_PER_MS / period_fs;
    uint64_t start = read(REG_COUNTER);
    while(((read(REG_COUNTER) - start) & counter_mask) < ticks){
        __asm__ volatile("pause");
    }
}
