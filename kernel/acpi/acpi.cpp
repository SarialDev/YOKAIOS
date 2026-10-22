#include "acpi/acpi.h"
#include "arch/x86_64/mmio.h"
#include "boot/multiboot2.h"
#include "lib/kprintf.h"

AcpiMadt acpi_madt;

namespace {

struct __attribute__((packed)) Rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    // ACPI 2.0+
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
};

struct __attribute__((packed)) MadtHeader {
    AcpiHeader header;
    uint32_t lapic_addr;
    uint32_t flags;
};

struct __attribute__((packed)) MadtEntry {
    uint8_t type;
    uint8_t length;
};

const uint8_t MADT_LAPIC = 0;
const uint8_t MADT_IOAPIC = 1;
const uint8_t MADT_OVERRIDE = 2;
const uint8_t MADT_LAPIC_ADDR = 5;

const uint64_t DIRECT_MAP_LIMIT = 1UL << 30;   // boot.S mapeaza 0-1 GiB la KERNEL_VMA

const AcpiHeader* root = nullptr;
bool use_xsdt = false;

const void* phys_to_virt(uint64_t phys, uint64_t size){
    if(phys + size <= DIRECT_MAP_LIMIT){
        return (const void*)(KERNEL_VMA + phys);
    }
    return mmio_map(phys, size);
}

bool checksum_ok(const void* p, uint32_t len){
    uint8_t sum = 0;
    for(uint32_t i = 0; i < len; i++){
        sum += ((const uint8_t*)p)[i];
    }
    return sum == 0;
}

bool sig_eq(const char* a, const char* b, int n){
    for(int i = 0; i < n; i++){
        if(a[i] != b[i]){
            return false;
        }
    }
    return true;
}

bool rsdp_valid(const Rsdp* r){
    if(!sig_eq(r->signature, "RSD PTR ", 8) || !checksum_ok(r, 20)){
        return false;
    }
    return r->revision < 2 || checksum_ok(r, r->length);
}

const Rsdp* scan_rsdp(uint64_t start, uint64_t len){
    for(uint64_t a = start; a < start + len; a += 16){
        const Rsdp* r = (const Rsdp*)(KERNEL_VMA + a);
        if(rsdp_valid(r)){
            return r;
        }
    }
    return nullptr;
}

const Rsdp* find_rsdp(){
    if(boot_info.rsdp && rsdp_valid((const Rsdp*)boot_info.rsdp)){
        return (const Rsdp*)boot_info.rsdp;
    }
    // Fallback: primul KiB din EBDA, apoi zona BIOS 0xE0000-0xFFFFF
    uint64_t ebda = (uint64_t)*(const uint16_t*)(KERNEL_VMA + 0x40E) << 4;
    const Rsdp* r = ebda ? scan_rsdp(ebda, 1024) : nullptr;
    return r ? r : scan_rsdp(0xE0000, 0x20000);
}

const AcpiHeader* map_table(uint64_t phys){
    const AcpiHeader* h = (const AcpiHeader*)phys_to_virt(phys, sizeof(AcpiHeader));
    return (const AcpiHeader*)phys_to_virt(phys, h->length);
}

void parse_madt(const MadtHeader* m){
    acpi_madt.lapic_addr = m->lapic_addr;
    const uint8_t* p = (const uint8_t*)(m + 1);
    const uint8_t* end = (const uint8_t*)m + m->header.length;
    while(p + sizeof(MadtEntry) <= end){
        const MadtEntry* e = (const MadtEntry*)p;
        if(e->length < 2){
            break;
        }
        switch(e->type){
            case MADT_LAPIC:
                if(acpi_madt.cpu_count < ACPI_MAX_CPUS){
                    AcpiCpu& c = acpi_madt.cpus[acpi_madt.cpu_count++];
                    c.acpi_id = p[2];
                    c.lapic_id = p[3];
                    c.flags = *(const uint32_t*)(p + 4);
                }
                break;
            case MADT_IOAPIC:
                if(acpi_madt.ioapic_count < ACPI_MAX_IOAPICS){
                    AcpiIoApic& io = acpi_madt.ioapics[acpi_madt.ioapic_count++];
                    io.id = p[2];
                    io.addr = *(const uint32_t*)(p + 4);
                    io.gsi_base = *(const uint32_t*)(p + 8);
                }
                break;
            case MADT_OVERRIDE:
                if(acpi_madt.override_count < ACPI_MAX_OVERRIDES){
                    AcpiOverride& o = acpi_madt.overrides[acpi_madt.override_count++];
                    o.irq = p[3];
                    o.gsi = *(const uint32_t*)(p + 4);
                    o.flags = *(const uint16_t*)(p + 8);
                }
                break;
            case MADT_LAPIC_ADDR:
                acpi_madt.lapic_addr = *(const uint64_t*)(p + 4);
                break;
        }
        p += e->length;
    }
}

}

const AcpiHeader* acpi_find_table(const char* signature){
    if(!root){
        return nullptr;
    }
    int entry_size = use_xsdt ? 8 : 4;
    int count = (root->length - sizeof(AcpiHeader)) / entry_size;
    const uint8_t* entries = (const uint8_t*)(root + 1);
    for(int i = 0; i < count; i++){
        uint64_t phys = use_xsdt ? *(const uint64_t*)(entries + i * 8)
                                 : *(const uint32_t*)(entries + i * 4);
        const AcpiHeader* h = map_table(phys);
        if(sig_eq(h->signature, signature, 4) && checksum_ok(h, h->length)){
            return h;
        }
    }
    return nullptr;
}

bool acpi_init(){
    const Rsdp* rsdp = find_rsdp();
    if(!rsdp){
        kprintf("ACPI: RSDP lipseste\n");
        return false;
    }
    use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_addr;
    root = map_table(use_xsdt ? rsdp->xsdt_addr : rsdp->rsdt_addr);
    if(!sig_eq(root->signature, use_xsdt ? "XSDT" : "RSDT", 4) || !checksum_ok(root, root->length)){
        kprintf("ACPI: %s invalid\n", use_xsdt ? "XSDT" : "RSDT");
        root = nullptr;
        return false;
    }
    kprintf("ACPI: rev %u, %s\n", rsdp->revision, use_xsdt ? "XSDT" : "RSDT");

    const MadtHeader* madt = (const MadtHeader*)acpi_find_table("APIC");
    if(!madt){
        kprintf("ACPI: MADT lipseste\n");
        return false;
    }
    parse_madt(madt);
    return true;
}
