#pragma once
#include "types.h"

// Controller USB xHCI (PCI 0C03, prog-if 30): reset, DCBAA, command ring, event ring pe MSI-X (MSI / polling ca
// rezerva), port reset, Enable Slot, Address Device. Porturile conectate la boot se enumereaza imediat;
// conectarile/deconectarile ulterioare le trateaza task-ul "usbd". Fara hub-uri si fara transferuri izocrone

struct __attribute__((packed)) XhciTrb {
    uint64_t param;
    uint32_t status;
    uint32_t control;           // bitul 0 = cycle, 15:10 = tipul
};

// Ring de transfer/comenzi: o pagina de TRB-uri, ultimul e Link inapoi la inceput
struct XhciRing {
    XhciTrb* trbs;
    uint64_t phys;
    uint16_t enq;
    uint8_t cycle;
};

struct XhciController;
struct UsbDevice;
struct UsbEndpoint;
struct UsbSetup;

// Inregistreaza driverul PCI (probe pe fiecare controller gasit)
void xhci_init();
int xhci_controller_count();

// Pentru usb.cpp. Intorc UsbStatus (usb.h)
int xhci_control(UsbDevice* dev, const UsbSetup& setup, void* data, bool wait);
// Evaluate Context pentru EP0 dupa ce se afla bMaxPacketSize0
bool xhci_set_ep0_mps(UsbDevice* dev, uint16_t mps);
// Configure Endpoint cu toate endpoint-urile din dev->eps (aloca ring-urile si bufferele)
bool xhci_configure_endpoints(UsbDevice* dev);
bool xhci_submit(UsbEndpoint* ep);
