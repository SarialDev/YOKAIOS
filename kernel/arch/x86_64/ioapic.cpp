#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/mmio.h"
#include "acpi/acpi.h"

namespace {

const uint32_t REG_SEL = 0x00;
const uint32_t REG_WIN = 0x10;
const uint32_t IOAPICVER = 0x01;
const uint32_t IOREDTBL = 0x10;

const uint32_t RED_ACTIVE_LOW = 1 << 13;
const uint32_t RED_LEVEL = 1 << 15;
const uint32_t RED_MASKED = 1 << 16;

// Flag-uri MPS INTI din override: 3 = activ jos / level
const uint16_t POLARITY_MASK = 0x3;
const uint16_t POLARITY_LOW = 0x3;
const uint16_t TRIGGER_MASK = 0xC;
const uint16_t TRIGGER_LEVEL = 0xC;

struct IoApic {
    volatile uint32_t* base;
    uint32_t gsi_base;
    uint32_t gsi_count;
};

IoApic ioapics[ACPI_MAX_IOAPICS];
int ioapic_count = 0;

uint32_t read(const IoApic& io, uint32_t reg){
    io.base[REG_SEL / 4] = reg;
    return io.base[REG_WIN / 4];
}

void write(const IoApic& io, uint32_t reg, uint32_t v){
    io.base[REG_SEL / 4] = reg;
    io.base[REG_WIN / 4] = v;
}

IoApic* find_ioapic(uint32_t gsi){
    for(int i = 0; i < ioapic_count; i++){
        if(gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].gsi_count){
            return &ioapics[i];
        }
    }
    return nullptr;
}

}

void ioapic_init(){
    for(int i = 0; i < acpi_madt.ioapic_count; i++){
        IoApic& io = ioapics[ioapic_count++];
        io.base = (volatile uint32_t*)mmio_map(acpi_madt.ioapics[i].addr, 0x20);
        io.gsi_base = acpi_madt.ioapics[i].gsi_base;
        io.gsi_count = ((read(io, IOAPICVER) >> 16) & 0xFF) + 1;
        for(uint32_t pin = 0; pin < io.gsi_count; pin++){
            write(io, IOREDTBL + pin * 2, RED_MASKED);
            write(io, IOREDTBL + pin * 2 + 1, 0);
        }
    }
}

bool ioapic_route(uint8_t irq, uint8_t vector, uint8_t lapic_id){
    // ISA implicit: identitate, activ sus, edge
    uint32_t gsi = irq;
    uint16_t flags = 0;
    for(int i = 0; i < acpi_madt.override_count; i++){
        if(acpi_madt.overrides[i].irq == irq){
            gsi = acpi_madt.overrides[i].gsi;
            flags = acpi_madt.overrides[i].flags;
            break;
        }
    }
    IoApic* io = find_ioapic(gsi);
    if(!io){
        return false;
    }
    uint32_t low = vector;
    if((flags & POLARITY_MASK) == POLARITY_LOW){
        low |= RED_ACTIVE_LOW;
    }
    if((flags & TRIGGER_MASK) == TRIGGER_LEVEL){
        low |= RED_LEVEL;
    }
    uint32_t pin = gsi - io->gsi_base;
    write(*io, IOREDTBL + pin * 2 + 1, (uint32_t)lapic_id << 24);
    write(*io, IOREDTBL + pin * 2, low);
    return true;
}
