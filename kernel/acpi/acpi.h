#pragma once
#include "types.h"

struct __attribute__((packed)) AcpiHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

const int ACPI_MAX_CPUS = 64;
const int ACPI_MAX_IOAPICS = 8;
const int ACPI_MAX_OVERRIDES = 16;

struct AcpiCpu {
    uint8_t acpi_id;
    uint8_t lapic_id;
    uint32_t flags;         // bit 0 activ, bit 1 poate fi pornit
};

struct AcpiIoApic {
    uint8_t id;
    uint32_t addr;
    uint32_t gsi_base;
};

struct AcpiOverride {
    uint8_t irq;            // sursa ISA
    uint32_t gsi;
    uint16_t flags;         // bit 0-1 polaritate, bit 2-3 trigger (MPS INTI)
};

struct AcpiMadt {
    uint64_t lapic_addr;
    AcpiCpu cpus[ACPI_MAX_CPUS];
    int cpu_count;
    AcpiIoApic ioapics[ACPI_MAX_IOAPICS];
    int ioapic_count;
    AcpiOverride overrides[ACPI_MAX_OVERRIDES];
    int override_count;
};

extern AcpiMadt acpi_madt;

// Gaseste RSDP, valideaza RSDT/XSDT si parseaza MADT
bool acpi_init();
// Tabelul cu semnatura data (ex. "APIC"), checksum valid, sau nullptr
const AcpiHeader* acpi_find_table(const char* signature);
