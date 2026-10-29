#include "tests/test_pci.h"
#include "drivers/pci/pci.h"
#include "arch/x86_64/idt.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "sync/atomic.h"

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_pci: %s (linia %d)", #cond, __LINE__); } while(0)

const uint16_t VENDOR_INTEL = 0x8086;
const uint16_t DEV_E1000 = 0x100E;
const uint16_t VENDOR_QEMU = 0x1234;
const uint16_t DEV_BOCHS_VGA = 0x1111;
const uint16_t DEV_EDU = 0x11E8;

const uint32_t EDU_IRQ_STATUS = 0x24 / 4;
const uint32_t EDU_IRQ_RAISE = 0x60 / 4;
const uint32_t EDU_IRQ_ACK = 0x64 / 4;

int probe_hits[4];
bool reject_next = true;

int probe_e1000(PciDevice*){ probe_hits[0]++; return 0; }
int probe_net(PciDevice*){ probe_hits[1]++; return 0; }
int probe_host(PciDevice*){ probe_hits[2]++; return 0; }
int probe_vga(PciDevice*){
    probe_hits[3]++;
    if(reject_next){
        reject_next = false;
        return -1;
    }
    return 0;
}

const PciDriver drv_e1000 = {"t-e1000", VENDOR_INTEL, DEV_E1000, PCI_ANY, PCI_ANY, probe_e1000};
const PciDriver drv_net = {"t-net", PCI_ANY, PCI_ANY, 2, PCI_ANY, probe_net};
const PciDriver drv_host = {"t-host", PCI_ANY, PCI_ANY, 6, 0, probe_host};
const PciDriver drv_vga = {"t-vga", VENDOR_QEMU, DEV_BOCHS_VGA, PCI_ANY, PCI_ANY, probe_vga};

volatile uint32_t* edu_regs;
volatile uint32_t msi_hits;
volatile uint32_t msi_vector;

void edu_handler(InterruptFrame* f){
    msi_vector = (uint32_t)f->vector;
    edu_regs[EDU_IRQ_ACK] = edu_regs[EDU_IRQ_STATUS];
    msi_hits = msi_hits + 1;
}

void msix_handler(InterruptFrame*){}

// Pana la 500 ms pentru n intreruperi; la boot IF e oprit, deci le porneste cat dureaza asteptarea
bool wait_hits(uint32_t n){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    uint64_t t0 = jiffies;
    while(msi_hits < n && jiffies - t0 < 500){
        __asm__ volatile("pause" : : : "memory");
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    return msi_hits >= n;
}

void check_enum(){
    pci_dump(true);
    CHECK(pci_device_count() >= 3);
    PciDevice* host = pci_device_at(0);
    CHECK(host->bdf == 0 && host->cls == 6 && host->subclass == 0);
    CHECK(pci_find_class(3, 0));
    PciDevice* nic = pci_find(VENDOR_INTEL, DEV_E1000);
    CHECK(nic && nic->cls == 2);
    CHECK(!nic->bars[0].io && nic->bars[0].size == 0x20000 && nic->bars[0].base);
    CHECK(nic->bars[1].io && nic->bars[1].size == 0x40);
    CHECK(pci_find(VENDOR_INTEL, DEV_E1000, 1) == nullptr);

    // ECAM si porturile trebuie sa vada acelasi config space
    for(int i = 0; i < pci_device_count(); i++){
        PciDevice* d = pci_device_at(i);
        CHECK(pci_cfg_read32(d->bdf, 0) == pci_cfg_read32_legacy(d->bdf, 0));
        CHECK(pci_cfg_read32(d->bdf, 0x10) == pci_cfg_read32_legacy(d->bdf, 0x10));
        CHECK(pci_cfg_read16(d->bdf, 0) == d->vendor && pci_cfg_read8(d->bdf, 2) == (d->device & 0xFF));
    }
    char line[96];
    pci_format(nic, line, sizeof(line));
    CHECK(line[2] == ':' && line[5] == '.');
    kprintf("[OK] test_pci enum\n");
}

void check_enable(){
    PciDevice* nic = pci_find(VENDOR_INTEL, DEV_E1000);
    pci_enable(nic, PCI_CMD_MEM | PCI_CMD_MASTER);
    uint16_t cmd = pci_cfg_read16(nic->bdf, 4);
    CHECK((cmd & (PCI_CMD_MEM | PCI_CMD_MASTER)) == (PCI_CMD_MEM | PCI_CMD_MASTER));
    volatile uint32_t* regs = (volatile uint32_t*)pci_map_bar(nic, 0);
    CHECK(regs);
    CHECK(regs[0x8 / 4] != 0xFFFFFFFF);     // STATUS
    CHECK(pci_map_bar(nic, 1) == nullptr);  // BAR I/O
    pci_disable(nic, PCI_CMD_MASTER);
    CHECK(!(pci_cfg_read16(nic->bdf, 4) & PCI_CMD_MASTER));
    kprintf("[OK] test_pci enable\n");
}

void check_drivers(){
    CHECK(pci_register_driver(&drv_e1000));
    CHECK(pci_register_driver(&drv_net));
    CHECK(pci_register_driver(&drv_host));
    CHECK(pci_register_driver(&drv_vga));
    PciDevice* nic = pci_find(VENDOR_INTEL, DEV_E1000);
    CHECK(nic->driver == &drv_e1000);       // ID bate clasa, chiar daca driverul de clasa se potriveste si el
    CHECK(probe_hits[0] == 1);
    CHECK(pci_device_at(0)->driver == &drv_host && probe_hits[2] == 1);
    // probe() respins: dispozitivul ramane liber, un driver nou il poate lua
    PciDevice* vga = pci_find(VENDOR_QEMU, DEV_BOCHS_VGA);
    CHECK(vga && vga->driver == nullptr && probe_hits[3] == 1);
    static const PciDriver drv_vga2 = {"t-vga2", PCI_ANY, PCI_ANY, 3, 0, probe_vga};
    CHECK(pci_register_driver(&drv_vga2));
    CHECK(vga->driver == &drv_vga2 && probe_hits[3] == 2);
    kprintf("[OK] test_pci drivers\n");
}

void check_msi(){
    PciDevice* edu = pci_find(VENDOR_QEMU, DEV_EDU);
    if(!edu){
        kprintf("test_pci: edu lipseste (-device edu), MSI sarit\n");
        return;
    }
    CHECK(edu->cap_msi);
    pci_enable(edu, PCI_CMD_MEM | PCI_CMD_MASTER);
    edu_regs = (volatile uint32_t*)pci_map_bar(edu, 0);
    CHECK(edu_regs && (edu_regs[0] & 0xFF) == 0xED);

    int vec = pci_msi_enable(edu, edu_handler);
    CHECK(vec >= IDT_VEC_MSI_FIRST && vec < IDT_VEC_MSI_END);
    uint16_t ctrl = pci_cfg_read16(edu->bdf, edu->cap_msi + 2);
    CHECK(ctrl & 1);
    CHECK((pci_cfg_read32(edu->bdf, edu->cap_msi + 4) & 0xFFF00000) == 0xFEE00000);
    CHECK(pci_cfg_read16(edu->bdf, edu->cap_msi + ((ctrl & 0x80) ? 12 : 8)) == vec);
    CHECK(pci_cfg_read16(edu->bdf, 4) & PCI_CMD_INTX_OFF);

    msi_hits = 0;
    edu_regs[EDU_IRQ_RAISE] = 1;
    CHECK(wait_hits(1));
    CHECK(msi_vector == (uint32_t)vec);
    edu_regs[EDU_IRQ_RAISE] = 2;
    CHECK(wait_hits(2));

    pci_msi_disable(edu);
    CHECK(!(pci_cfg_read16(edu->bdf, edu->cap_msi + 2) & 1));
    kprintf("[OK] test_pci msi (vector %d)\n", vec);
}

void check_msix(){
    PciDevice* dev = nullptr;
    for(int i = 0; i < pci_device_count() && !dev; i++){
        if(pci_device_at(i)->cap_msix){
            dev = pci_device_at(i);
        }
    }
    if(!dev){
        kprintf("test_pci: nici un dispozitiv MSI-X, sarit\n");
        return;
    }
    CHECK(pci_msix_enable(dev) && dev->msix_count >= 1);
    CHECK(pci_cfg_read16(dev->bdf, dev->cap_msix + 2) & 0x8000);
    CHECK(dev->msix_table[3] & 1);          // intrarile pornesc mascate
    int vec = pci_msix_setup(dev, 0, msix_handler);
    CHECK(vec >= IDT_VEC_MSI_FIRST);
    volatile uint32_t* e = dev->msix_table;
    CHECK((e[0] & 0xFFF00000) == 0xFEE00000 && e[1] == 0);
    CHECK((int)e[2] == vec);
    CHECK(!(e[3] & 1));
    CHECK(pci_msix_setup(dev, dev->msix_count, msix_handler) < 0);
    pci_msix_disable(dev);
    CHECK(!(pci_cfg_read16(dev->bdf, dev->cap_msix + 2) & 0x8000));
    kprintf("[OK] test_pci msix (%d intrari)\n", (int)dev->msix_count);
}

}

void test_pci_run(){
    check_enum();
    check_enable();
    check_drivers();
    check_msi();
    check_msix();
    kprintf("[OK] test_pci (%d devices)\n", pci_device_count());
}
