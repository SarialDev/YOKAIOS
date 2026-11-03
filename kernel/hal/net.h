#pragma once
#include "hal/hal.h"

// Contoare ale unui dispozitiv de retea (monotone, de la pornirea driverului)
struct NetStats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_errors;     // cadre cu eroare de hardware (CRC, depasire etc.)
    uint64_t rx_dropped;    // pierdute: inel plin sau fara buffer
    uint64_t rx_arp;        // cadre cu ethertype 0x0806 (ARP) primite
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;     // refuzate (prea lungi) sau cu eroare de transmisie
    uint64_t tx_dropped;    // coada de transmisie plina
    uint64_t irqs;
    uint64_t link_changes;
};

class NetDevice;

// Apelat pentru fiecare cadru primit, din contextul de intrerupere (sau din task-ul de interogare); frame
// arata direct in inelul RX si e valabil doar cat dureaza apelul: handlerul copiaza ce pastreaza.
// Nu are voie sa blocheze; send() pe acelasi dispozitiv e permis.
typedef void (*NetRxHandler)(NetDevice* dev, const void* frame, size_t len, void* ctx);

class NetDevice : public HalDevice {
public:
    virtual void mac(uint8_t out[6]) const = 0;
    virtual uint32_t mtu() const = 0;
    virtual bool link_up() = 0;
    // 0 la succes, negativ la eroare (ex. coada plina)
    virtual int send(const void* frame, size_t len) = 0;
    // Lungimea cadrului primit, 0 daca nu e niciunul, negativ la eroare
    virtual int recv(void* buf, size_t max) = 0;
    // Cu handler setat, cadrele merg la handler (nu mai raman pentru recv); nullptr = inapoi la recv.
    // Implicit nu face nimic (dispozitiv doar cu recv)
    virtual void set_rx_handler(NetRxHandler h, void* ctx){ (void)h; (void)ctx; }
    // Copie a contoarelor; implicit zero
    virtual void stats(NetStats& out){ out = NetStats{}; }
protected:
    constexpr NetDevice() = default;
};

inline NetDevice* hal_net(int idx){
    return static_cast<NetDevice*>(hal_get(HalKind::Net, idx));
}
