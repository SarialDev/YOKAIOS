#include "drivers/pci/pci.h"
#include "acpi/acpi.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/mmio.h"
#include "hal/irqchip.h"
#include "sync/spinlock.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "io.h"

namespace {

const uint16_t CFG_VENDOR = 0x00;
const uint16_t CFG_COMMAND = 0x04;
const uint16_t CFG_STATUS = 0x06;
const uint16_t CFG_REVISION = 0x08;
const uint16_t CFG_HEADER = 0x0E;
const uint16_t CFG_BAR0 = 0x10;
const uint16_t CFG_CAP_PTR = 0x34;
const uint16_t CFG_IRQ_LINE = 0x3C;
const uint16_t CFG_PRIMARY_BUS = 0x18;
const uint16_t CFG_SECONDARY_BUS = 0x19;
const uint16_t CFG_SUBORDINATE_BUS = 0x1A;

const uint8_t CAP_MSI = 0x05;
const uint8_t CAP_PCIE = 0x10;
const uint8_t CAP_MSIX = 0x11;

const uint32_t MSI_ADDR_BASE = 0xFEE00000;

struct __attribute__((packed)) McfgEntry {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
};

bool ecam_on = false;
McfgEntry ecam;
volatile uint8_t* ecam_bus[256];
Spinlock cf8_lock("pci-cf8");

PciDevice devices[PCI_MAX_DEVICES];
int device_total = 0;
bool bus_seen[256];
int last_bus = 0;

const PciDriver* drivers[PCI_MAX_DRIVERS];
int driver_total = 0;

Spinlock vec_lock("pci-vec");
uint8_t next_vector = IDT_VEC_MSI_FIRST;

uint16_t make_bdf(int bus, int dev, int fn){
    return (uint16_t)(bus << 8 | dev << 3 | fn);
}

bool in_ecam(uint16_t bdf){
    uint8_t bus = bdf >> 8;
    return ecam_on && bus >= ecam.start_bus && bus <= ecam.end_bus;
}

// Un MiB de config space per bus; mmio_map reutilizeaza paginile de 2 MiB deja mapate
volatile uint8_t* ecam_ptr(uint16_t bdf, uint16_t off){
    uint8_t bus = bdf >> 8;
    if(!ecam_bus[bus]){
        ecam_bus[bus] = (volatile uint8_t*)mmio_map(ecam.base + ((uint64_t)(bus - ecam.start_bus) << 20), 1UL << 20);
    }
    return ecam_bus[bus] + (((uint32_t)(bdf & 0xFF) << 12) | off);
}

uint32_t legacy_addr(uint16_t bdf, uint16_t off){
    return 0x80000000u | ((uint32_t)bdf << 8) | (off & 0xFC);
}

uint32_t legacy_read32(uint16_t bdf, uint16_t off){
    if(off > 0xFC){
        return 0xFFFFFFFF;
    }
    SpinlockGuard g(cf8_lock);
    outl(0xCF8, legacy_addr(bdf, off));
    return inl(0xCFC);
}

bool is_bridge(const PciDevice& d){
    return d.header_type == 1 && d.cls == 6 && d.subclass == 4;
}

struct Name {
    uint16_t a, b;
    const char* text;
};

const Name vendor_names[] = {
    {0x8086, 0, "Intel"}, {0x1234, 0, "QEMU"}, {0x1AF4, 0, "Red Hat virtio"}, {0x1B36, 0, "Red Hat"},
    {0x10EC, 0, "Realtek"}, {0x1022, 0, "AMD"}, {0x10DE, 0, "NVIDIA"}, {0x15AD, 0, "VMware"},
};

const Name device_names[] = {
    {0x8086, 0x1237, "440FX Host bridge"}, {0x8086, 0x7000, "PIIX3 ISA bridge"},
    {0x8086, 0x7010, "PIIX3 IDE"}, {0x8086, 0x7113, "PIIX4 ACPI"},
    {0x8086, 0x29C0, "Q35 Host bridge"}, {0x8086, 0x2918, "ICH9 LPC bridge"},
    {0x8086, 0x2922, "ICH9 SATA AHCI"}, {0x8086, 0x2930, "ICH9 SMBus"},
    {0x8086, 0x100E, "82540EM Gigabit Ethernet"}, {0x8086, 0x10D3, "82574L Gigabit Ethernet"},
    {0x1234, 0x1111, "Bochs VGA"}, {0x1234, 0x11E8, "QEMU edu"},
    {0x1AF4, 0x1000, "virtio-net"}, {0x1AF4, 0x1041, "virtio-net"},
    {0x1AF4, 0x1050, "virtio-gpu"}, {0x1B36, 0x0010, "NVMe"},
};

struct ClassName {
    uint8_t cls, sub;      // sub 0xFF = oricare
    const char* text;
};

const ClassName class_names[] = {
    {0x00, 0xFF, "Unclassified device"},
    {0x01, 0x00, "SCSI storage controller"}, {0x01, 0x01, "IDE interface"}, {0x01, 0x06, "SATA controller"},
    {0x01, 0x08, "Non-Volatile memory controller"}, {0x01, 0xFF, "Mass storage controller"},
    {0x02, 0x00, "Ethernet controller"}, {0x02, 0xFF, "Network controller"},
    {0x03, 0x00, "VGA compatible controller"}, {0x03, 0xFF, "Display controller"},
    {0x04, 0xFF, "Multimedia controller"}, {0x05, 0xFF, "Memory controller"},
    {0x06, 0x00, "Host bridge"}, {0x06, 0x01, "ISA bridge"}, {0x06, 0x04, "PCI bridge"},
    {0x06, 0x80, "Bridge"}, {0x06, 0xFF, "Bridge"},
    {0x07, 0xFF, "Communication controller"}, {0x08, 0xFF, "System peripheral"},
    {0x09, 0xFF, "Input device controller"}, {0x0C, 0x03, "USB controller"}, {0x0C, 0xFF, "Serial bus controller"},
    {0xFF, 0xFF, "Unassigned class"},
};

const char* class_name(uint8_t cls, uint8_t sub){
    const char* any = nullptr;
    for(const ClassName& c : class_names){
        if(c.cls != cls){
            continue;
        }
        if(c.sub == sub){
            return c.text;
        }
        if(c.sub == 0xFF){
            any = c.text;
        }
    }
    return any ? any : "Unknown class";
}

const char* find_name(const Name* table, int n, uint16_t a, uint16_t b){
    for(int i = 0; i < n; i++){
        if(table[i].a == a && table[i].b == b){
            return table[i].text;
        }
    }
    return nullptr;
}

struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    void put(char c){
        if(len + 1 < cap){
            buf[len++] = c;
        }
    }
    void str(const char* s){
        while(*s){
            put(*s++);
        }
    }
    // digits == 0: cifrele strict necesare
    void hex(uint64_t v, int digits){
        int nd = digits;
        if(!nd){
            nd = 1;
            for(uint64_t t = v >> 4; t; t >>= 4){
                nd++;
            }
        }
        for(int i = nd - 1; i >= 0; i--){
            put("0123456789abcdef"[(v >> (i * 4)) & 0xF]);
        }
    }
    void end(){
        if(cap){
            buf[len] = '\0';
        }
    }
};

void decode_bars(PciDevice& d, int count){
    uint16_t cmd = pci_cfg_read16(d.bdf, CFG_COMMAND);
    pci_cfg_write16(d.bdf, CFG_COMMAND, cmd & ~(PCI_CMD_IO | PCI_CMD_MEM));
    for(int i = 0; i < count; i++){
        uint16_t off = CFG_BAR0 + i * 4;
        uint32_t lo = pci_cfg_read32(d.bdf, off);
        pci_cfg_write32(d.bdf, off, 0xFFFFFFFF);
        uint32_t m = pci_cfg_read32(d.bdf, off);
        pci_cfg_write32(d.bdf, off, lo);
        if(m == 0 || m == 0xFFFFFFFF){
            continue;
        }
        PciBar& b = d.bars[i];
        if(lo & 1){
            uint32_t mask = m & ~3u;
            if((m >> 16) == 0){
                mask |= 0xFFFF0000u;
            }
            b.io = true;
            b.base = lo & ~3u;
            b.size = (uint32_t)(~mask + 1);
            continue;
        }
        uint32_t mask = m & ~0xFu;
        b.prefetch = lo & 8;
        b.base = lo & ~0xFu;
        if(((lo >> 1) & 3) == 2 && i + 1 < count){
            uint32_t hi = pci_cfg_read32(d.bdf, off + 4);
            pci_cfg_write32(d.bdf, off + 4, 0xFFFFFFFF);
            uint32_t mh = pci_cfg_read32(d.bdf, off + 4);
            pci_cfg_write32(d.bdf, off + 4, hi);
            b.mem64 = true;
            b.base |= (uint64_t)hi << 32;
            b.size = ~(((uint64_t)mh << 32) | mask) + 1;
            i++;
        } else if(mask){
            b.size = (uint32_t)(~mask + 1);
        }
    }
    pci_cfg_write16(d.bdf, CFG_COMMAND, cmd);
}

void find_caps(PciDevice& d){
    if(!(pci_cfg_read16(d.bdf, CFG_STATUS) & 0x10)){
        return;
    }
    uint8_t ptr = pci_cfg_read8(d.bdf, CFG_CAP_PTR) & ~3;
    for(int guard = 0; ptr >= 0x40 && guard < 48; guard++){
        uint8_t id = pci_cfg_read8(d.bdf, ptr);
        if(id == CAP_MSI){
            d.cap_msi = ptr;
        } else if(id == CAP_MSIX){
            d.cap_msix = ptr;
        } else if(id == CAP_PCIE){
            d.cap_pcie = ptr;
        }
        ptr = pci_cfg_read8(d.bdf, ptr + 1) & ~3;
    }
}

void scan_bus(int bus);

void add_device(uint16_t bdf){
    if(device_total >= PCI_MAX_DEVICES){
        return;
    }
    PciDevice& d = devices[device_total++];
    uint32_t id = pci_cfg_read32(bdf, CFG_VENDOR);
    uint32_t cls = pci_cfg_read32(bdf, CFG_REVISION);
    d.bdf = bdf;
    d.vendor = id & 0xFFFF;
    d.device = id >> 16;
    d.revision = cls & 0xFF;
    d.progif = (cls >> 8) & 0xFF;
    d.subclass = (cls >> 16) & 0xFF;
    d.cls = cls >> 24;
    d.header_type = pci_cfg_read8(bdf, CFG_HEADER) & 0x7F;
    d.irq_line = pci_cfg_read8(bdf, CFG_IRQ_LINE);
    d.irq_pin = pci_cfg_read8(bdf, CFG_IRQ_LINE + 1);
    find_caps(d);
    if(d.header_type == 0){
        decode_bars(d, 6);
    } else if(d.header_type == 1){
        decode_bars(d, 2);
    }
    if(!is_bridge(d)){
        return;
    }
    int sec = pci_cfg_read8(bdf, CFG_SECONDARY_BUS);
    int bus = bdf >> 8;
    if(sec <= bus || bus_seen[sec]){
        // Firmware nu a numerotat bridge-ul: ia urmatorul bus liber
        sec = last_bus + 1;
        if(sec > 255){
            return;
        }
        pci_cfg_write8(bdf, CFG_PRIMARY_BUS, bus);
        pci_cfg_write8(bdf, CFG_SECONDARY_BUS, sec);
        pci_cfg_write8(bdf, CFG_SUBORDINATE_BUS, 0xFF);
    }
    d.secondary_bus = sec;
    scan_bus(sec);
    pci_cfg_write8(bdf, CFG_SUBORDINATE_BUS, last_bus);
}

void scan_bus(int bus){
    if(bus_seen[bus]){
        return;
    }
    bus_seen[bus] = true;
    if(bus > last_bus){
        last_bus = bus;
    }
    for(int dev = 0; dev < 32; dev++){
        uint16_t first = make_bdf(bus, dev, 0);
        if(pci_cfg_read16(first, CFG_VENDOR) == 0xFFFF){
            continue;
        }
        int fns = (pci_cfg_read8(first, CFG_HEADER) & 0x80) ? 8 : 1;
        for(int fn = 0; fn < fns; fn++){
            uint16_t bdf = make_bdf(bus, dev, fn);
            if(pci_cfg_read16(bdf, CFG_VENDOR) != 0xFFFF){
                add_device(bdf);
            }
        }
    }
}

bool driver_matches(const PciDriver* drv, const PciDevice* d){
    if(drv->vendor != PCI_ANY){
        return drv->vendor == d->vendor && (drv->device == PCI_ANY || drv->device == d->device);
    }
    return drv->cls != PCI_ANY && drv->cls == d->cls && (drv->subclass == PCI_ANY || drv->subclass == d->subclass);
}

bool msi_message(uint64_t& addr, uint32_t& data, uint8_t vector){
    if(!hal_irqchip_smp()){
        return false;
    }
    addr = MSI_ADDR_BASE | ((uint64_t)hal_irqchip()->cpu_id() << 12);
    data = vector;      // fixed, edge
    return true;
}

}

bool pci_ecam_active(){ return ecam_on; }
int pci_device_count(){ return device_total; }

PciDevice* pci_device_at(int idx){
    return idx >= 0 && idx < device_total ? &devices[idx] : nullptr;
}

PciDevice* pci_find(uint16_t vendor, uint16_t device, int idx){
    for(int i = 0; i < device_total; i++){
        if(devices[i].vendor == vendor && devices[i].device == device && idx-- == 0){
            return &devices[i];
        }
    }
    return nullptr;
}

PciDevice* pci_find_class(uint8_t cls, uint8_t subclass, int idx){
    for(int i = 0; i < device_total; i++){
        if(devices[i].cls == cls && devices[i].subclass == subclass && idx-- == 0){
            return &devices[i];
        }
    }
    return nullptr;
}

uint8_t pci_cfg_read8(uint16_t bdf, uint16_t off){
    if(in_ecam(bdf)){
        return *ecam_ptr(bdf, off);
    }
    return (uint8_t)(legacy_read32(bdf, off & ~3) >> ((off & 3) * 8));
}

uint16_t pci_cfg_read16(uint16_t bdf, uint16_t off){
    if(in_ecam(bdf)){
        return *(volatile uint16_t*)ecam_ptr(bdf, off & ~1);
    }
    return (uint16_t)(legacy_read32(bdf, off & ~3) >> ((off & 2) * 8));
}

uint32_t pci_cfg_read32(uint16_t bdf, uint16_t off){
    if(in_ecam(bdf)){
        return *(volatile uint32_t*)ecam_ptr(bdf, off & ~3);
    }
    return legacy_read32(bdf, off);
}

uint32_t pci_cfg_read32_legacy(uint16_t bdf, uint16_t off){
    return legacy_read32(bdf, off);
}

void pci_cfg_write8(uint16_t bdf, uint16_t off, uint8_t v){
    if(in_ecam(bdf)){
        *ecam_ptr(bdf, off) = v;
    } else if(off <= 0xFF){
        SpinlockGuard g(cf8_lock);
        outl(0xCF8, legacy_addr(bdf, off));
        outb(0xCFC + (off & 3), v);
    }
}

void pci_cfg_write16(uint16_t bdf, uint16_t off, uint16_t v){
    if(in_ecam(bdf)){
        *(volatile uint16_t*)ecam_ptr(bdf, off & ~1) = v;
    } else if(off <= 0xFF){
        SpinlockGuard g(cf8_lock);
        outl(0xCF8, legacy_addr(bdf, off));
        outw(0xCFC + (off & 2), v);
    }
}

void pci_cfg_write32(uint16_t bdf, uint16_t off, uint32_t v){
    if(in_ecam(bdf)){
        *(volatile uint32_t*)ecam_ptr(bdf, off & ~3) = v;
    } else if(off <= 0xFC){
        SpinlockGuard g(cf8_lock);
        outl(0xCF8, legacy_addr(bdf, off));
        outl(0xCFC, v);
    }
}

void pci_enable(PciDevice* dev, uint16_t cmd_bits){
    uint16_t cmd = pci_cfg_read16(dev->bdf, CFG_COMMAND);
    pci_cfg_write16(dev->bdf, CFG_COMMAND, cmd | cmd_bits);
}

void pci_disable(PciDevice* dev, uint16_t cmd_bits){
    uint16_t cmd = pci_cfg_read16(dev->bdf, CFG_COMMAND);
    pci_cfg_write16(dev->bdf, CFG_COMMAND, cmd & ~cmd_bits);
}

void* pci_map_bar(PciDevice* dev, int bar){
    if(bar < 0 || bar >= PCI_BAR_COUNT){
        return nullptr;
    }
    const PciBar& b = dev->bars[bar];
    if(!b.size || b.io || !b.base){
        return nullptr;
    }
    return mmio_map(b.base, b.size);
}

bool pci_register_driver(const PciDriver* drv){
    if(!drv || !drv->probe || driver_total >= PCI_MAX_DRIVERS){
        return false;
    }
    drivers[driver_total++] = drv;
    for(int i = 0; i < device_total; i++){
        if(!devices[i].driver && driver_matches(drv, &devices[i]) && drv->probe(&devices[i]) == 0){
            devices[i].driver = drv;
        }
    }
    return true;
}

int pci_alloc_vector(InterruptHandler handler){
    uint8_t vec;
    {
        SpinlockGuard g(vec_lock);
        if(next_vector >= IDT_VEC_MSI_END){
            return -1;
        }
        vec = next_vector++;
    }
    irq_register(vec, handler);
    return vec;
}

int pci_msi_enable(PciDevice* dev, InterruptHandler handler){
    uint16_t cap = dev->cap_msi;
    uint64_t addr;
    uint32_t data;
    if(!cap || dev->msi_vector){
        return -1;
    }
    int vec = hal_irqchip_smp() ? pci_alloc_vector(handler) : -1;
    if(vec < 0 || !msi_message(addr, data, (uint8_t)vec)){
        return -1;
    }
    uint16_t ctrl = pci_cfg_read16(dev->bdf, cap + 2);
    bool is64 = ctrl & 0x80;
    pci_cfg_write32(dev->bdf, cap + 4, (uint32_t)addr);
    if(is64){
        pci_cfg_write32(dev->bdf, cap + 8, (uint32_t)(addr >> 32));
    }
    pci_cfg_write16(dev->bdf, cap + (is64 ? 12 : 8), (uint16_t)data);
    if(ctrl & 0x100){
        pci_cfg_write32(dev->bdf, cap + (is64 ? 16 : 12), 0);
    }
    // Un singur mesaj (Multiple Message Enable = 0), MSI activ, INTx oprit
    pci_cfg_write16(dev->bdf, cap + 2, (ctrl & ~0x70) | 1);
    pci_enable(dev, PCI_CMD_INTX_OFF);
    dev->msi_vector = (uint8_t)vec;
    return vec;
}

void pci_msi_disable(PciDevice* dev){
    if(!dev->cap_msi || !dev->msi_vector){
        return;
    }
    uint16_t ctrl = pci_cfg_read16(dev->bdf, dev->cap_msi + 2);
    pci_cfg_write16(dev->bdf, dev->cap_msi + 2, ctrl & ~1);
    pci_disable(dev, PCI_CMD_INTX_OFF);
    irq_register(dev->msi_vector, nullptr);
    dev->msi_vector = 0;
}

bool pci_msix_enable(PciDevice* dev){
    uint16_t cap = dev->cap_msix;
    if(!cap || dev->msix_table){
        return false;
    }
    uint16_t ctrl = pci_cfg_read16(dev->bdf, cap + 2);
    uint32_t table = pci_cfg_read32(dev->bdf, cap + 4);
    int count = (ctrl & 0x7FF) + 1;
    const PciBar& b = dev->bars[table & 7];
    if(!b.size || b.io || !b.base){
        return false;
    }
    pci_enable(dev, PCI_CMD_MEM);
    volatile uint32_t* t = (volatile uint32_t*)mmio_map(b.base + (table & ~7u), (uint64_t)count * 16);
    for(int i = 0; i < count; i++){
        t[i * 4 + 3] = 1;       // Vector Control: masked
    }
    // Enable, function mask sters; INTx oprit
    pci_cfg_write16(dev->bdf, cap + 2, (ctrl | 0x8000) & ~0x4000);
    pci_enable(dev, PCI_CMD_INTX_OFF);
    dev->msix_table = t;
    dev->msix_count = (uint16_t)count;
    return true;
}

int pci_msix_setup(PciDevice* dev, int entry, InterruptHandler handler){
    uint64_t addr;
    uint32_t data;
    if(!dev->msix_table || entry < 0 || entry >= dev->msix_count){
        return -1;
    }
    int vec = hal_irqchip_smp() ? pci_alloc_vector(handler) : -1;
    if(vec < 0 || !msi_message(addr, data, (uint8_t)vec)){
        return -1;
    }
    volatile uint32_t* e = dev->msix_table + entry * 4;
    e[3] = 1;
    e[0] = (uint32_t)addr;
    e[1] = (uint32_t)(addr >> 32);
    e[2] = data;
    e[3] = 0;
    return vec;
}

void pci_msix_disable(PciDevice* dev){
    if(!dev->msix_table){
        return;
    }
    for(int i = 0; i < dev->msix_count; i++){
        dev->msix_table[i * 4 + 3] = 1;
    }
    uint16_t ctrl = pci_cfg_read16(dev->bdf, dev->cap_msix + 2);
    pci_cfg_write16(dev->bdf, dev->cap_msix + 2, ctrl & ~0x8000);
    pci_disable(dev, PCI_CMD_INTX_OFF);
    dev->msix_table = nullptr;
}

void pci_init(){
    device_total = 0;
    const AcpiHeader* mcfg = acpi_find_table("MCFG");
    if(mcfg && mcfg->length >= sizeof(AcpiHeader) + 8 + sizeof(McfgEntry)){
        const McfgEntry* e = (const McfgEntry*)((const uint8_t*)mcfg + sizeof(AcpiHeader) + 8);
        int n = (mcfg->length - sizeof(AcpiHeader) - 8) / sizeof(McfgEntry);
        for(int i = 0; i < n; i++){
            if(e[i].segment == 0 && e[i].start_bus == 0){
                ecam = e[i];
                ecam_on = true;
                break;
            }
        }
    }
    // Fara ECAM, config space exista doar prin porturi; fara nici un host bridge scanarea nu gaseste nimic
    scan_bus(0);
    kprintf("[OK] pci %s, %d devices\n", ecam_on ? "ecam" : "legacy", device_total);
}

void pci_format(const PciDevice* d, char* buf, size_t n){
    Out o{buf, n};
    o.hex(pci_bus(d), 2);
    o.put(':');
    o.hex(pci_slot(d), 2);
    o.put('.');
    o.hex(pci_fn(d), 1);
    o.put(' ');
    o.str(class_name(d->cls, d->subclass));
    o.str(" [");
    o.hex(d->cls, 2);
    o.hex(d->subclass, 2);
    o.str("]: ");
    o.hex(d->vendor, 4);
    o.put(':');
    o.hex(d->device, 4);
    const char* name = find_name(device_names, sizeof(device_names) / sizeof(device_names[0]), d->vendor, d->device);
    if(!name){
        name = find_name(vendor_names, sizeof(vendor_names) / sizeof(vendor_names[0]), d->vendor, 0);
    }
    if(name){
        o.put(' ');
        o.str(name);
    }
    o.str(" (rev ");
    o.hex(d->revision, 2);
    o.put(')');
    o.end();
}

bool pci_format_bar(const PciDevice* d, int bar, char* buf, size_t n){
    if(bar < 0 || bar >= PCI_BAR_COUNT || !d->bars[bar].size){
        return false;
    }
    const PciBar& b = d->bars[bar];
    Out o{buf, n};
    o.str("  BAR");
    o.hex((uint64_t)bar, 1);
    o.str(b.io ? ": io " : b.mem64 ? ": mem64 " : ": mem ");
    if(b.prefetch){
        o.str("prefetch ");
    }
    o.str("0x");
    o.hex(b.base, 0);
    o.str(" size 0x");
    o.hex(b.size, 0);
    o.end();
    return true;
}

void pci_dump(bool verbose){
    char line[96];
    for(int i = 0; i < device_total; i++){
        pci_format(&devices[i], line, sizeof(line));
        kprintf("%s\n", line);
        if(verbose){
            for(int b = 0; b < PCI_BAR_COUNT; b++){
                if(pci_format_bar(&devices[i], b, line, sizeof(line))){
                    kprintf("%s\n", line);
                }
            }
            if(devices[i].cap_msi || devices[i].cap_msix){
                kprintf("  caps:%s%s\n", devices[i].cap_msi ? " MSI" : "", devices[i].cap_msix ? " MSI-X" : "");
            }
        }
    }
}
