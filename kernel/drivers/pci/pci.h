#pragma once
#include "types.h"
#include "interrupt.h"

const uint16_t PCI_ANY = 0xFFFF;
const int PCI_MAX_DEVICES = 128;
const int PCI_MAX_DRIVERS = 16;
const int PCI_BAR_COUNT = 6;

const uint16_t PCI_CMD_IO = 1 << 0;
const uint16_t PCI_CMD_MEM = 1 << 1;
const uint16_t PCI_CMD_MASTER = 1 << 2;
const uint16_t PCI_CMD_INTX_OFF = 1 << 10;

// size == 0: BAR absent (sau partea de sus a unui BAR de 64 biti)
struct PciBar {
    uint64_t base;
    uint64_t size;
    bool io;
    bool mem64;
    bool prefetch;
};

struct PciDriver;

struct PciDevice {
    uint16_t bdf;           // bus << 8 | dev << 3 | fn
    uint16_t vendor;
    uint16_t device;
    uint8_t cls;
    uint8_t subclass;
    uint8_t progif;
    uint8_t revision;
    uint8_t header_type;    // fara bitul multifunction
    uint8_t irq_line;
    uint8_t irq_pin;
    uint8_t secondary_bus;  // doar bridge
    uint8_t cap_msi;        // offsetul capabilitatii in config space, 0 = lipseste
    uint8_t cap_msix;
    uint8_t cap_pcie;
    PciBar bars[PCI_BAR_COUNT];
    const PciDriver* driver;
    void* drvdata;
    volatile uint32_t* msix_table;
    uint16_t msix_count;
    uint8_t msi_vector;     // 0 = MSI inactiv
};

inline uint8_t pci_bus(const PciDevice* d){ return d->bdf >> 8; }
inline uint8_t pci_slot(const PciDevice* d){ return (d->bdf >> 3) & 31; }
inline uint8_t pci_fn(const PciDevice* d){ return d->bdf & 7; }

// Potrivire: vendor (+ device) daca vendor != PCI_ANY, altfel cls (+ subclass). Primul driver inregistrat care se potriveste castiga
struct PciDriver {
    const char* name;
    uint16_t vendor;
    uint16_t device;
    uint16_t cls;
    uint16_t subclass;
    int (*probe)(PciDevice* dev);   // 0 = driverul a preluat dispozitivul
};

// Cauta MCFG (ECAM) in ACPI, altfel porturile 0xCF8/0xCFC; scaneaza recursiv bridge-urile
void pci_init();
bool pci_ecam_active();

int pci_device_count();
PciDevice* pci_device_at(int idx);
// idx-ul-lea dispozitiv cu vendor:device
PciDevice* pci_find(uint16_t vendor, uint16_t device, int idx = 0);
PciDevice* pci_find_class(uint8_t cls, uint8_t subclass, int idx = 0);

uint8_t pci_cfg_read8(uint16_t bdf, uint16_t off);
uint16_t pci_cfg_read16(uint16_t bdf, uint16_t off);
uint32_t pci_cfg_read32(uint16_t bdf, uint16_t off);
void pci_cfg_write8(uint16_t bdf, uint16_t off, uint8_t v);
void pci_cfg_write16(uint16_t bdf, uint16_t off, uint16_t v);
void pci_cfg_write32(uint16_t bdf, uint16_t off, uint32_t v);
// Acces prin porturile 0xCF8/0xCFC, indiferent de ECAM (pentru verificare)
uint32_t pci_cfg_read32_legacy(uint16_t bdf, uint16_t off);

// Seteaza bitii din PCI_CMD_* in registrul Command
void pci_enable(PciDevice* dev, uint16_t cmd_bits);
void pci_disable(PciDevice* dev, uint16_t cmd_bits);
// Mapeaza un BAR de memorie (necache); nullptr pentru I/O sau BAR absent
void* pci_map_bar(PciDevice* dev, int bar);

bool pci_register_driver(const PciDriver* drv);

// Aloca un vector de intrerupere (IDT_VEC_MSI_FIRST..) si il leaga la handler; -1 daca nu mai sunt
int pci_alloc_vector(InterruptHandler handler);

// MSI cu un singur mesaj, catre CPU-ul curent. Intoarce vectorul sau -1 (fara MSI / fara LAPIC)
int pci_msi_enable(PciDevice* dev, InterruptHandler handler);
void pci_msi_disable(PciDevice* dev);
// MSI-X: enable mapeaza tabelul si mascheaza toate intrarile; setup programeaza o intrare si o demascheaza
bool pci_msix_enable(PciDevice* dev);
int pci_msix_setup(PciDevice* dev, int entry, InterruptHandler handler);
void pci_msix_disable(PciDevice* dev);

// "00:01.0 VGA compatible controller [0300]: 1234:1111 Bochs VGA (rev 02)"
void pci_format(const PciDevice* dev, char* buf, size_t n);
// "  BAR0: mem64 prefetch 0xfebc0000 size 0x20000"; false daca BAR-ul lipseste
bool pci_format_bar(const PciDevice* dev, int bar, char* buf, size_t n);
// lspci in kprintf, cu -v si BAR-urile
void pci_dump(bool verbose);
