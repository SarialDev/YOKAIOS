#pragma once
#include "types.h"
#include "hal/net.h"

// Driver Realtek RTL8139 (PCI 10ec:8139, "rtl8139" din QEMU): reset, MAC din IDR0..5, buffer RX circular de 8 KiB + 16
// (cu WRAP: dupa capat hardware-ul continua liniar, de aceea alocarea e mai mare), 4 descriptori TX cu buffere de 2 KiB,
// registre prin BAR1 (memorie). MSI daca placa are capabilitatea, altfel interogare (QEMU nu da MSI pe rtl8139; INTx
// nu e rutat inca): cadrele se iau la recv() / stats() / din task-ul "rtl8139p" (1 ms), creat la primul set_rx_handler.
// Fara handler, cadrele primite se copiaza intr-o coada software de 32 de cadre pentru recv() (inelul hardware se
// elibereaza imediat). MTU 1500, fara offload/VLAN.
// Placile apar in HAL (HalKind::Net) ca eth<N>, numerotate dupa placile e1000; pana la 2 placi.

// Inregistreaza driverul PCI; probe-ul ruleaza imediat pe placile deja enumerate
void rtl8139_init();
int rtl8139_count();
NetDevice* rtl8139_dev(int idx);
// 0 = interogare, 1 = MSI
int rtl8139_irq_mode(int idx);
// Ia cadrele sosite (numara, apeleaza handlerul / le pune in coada) si elibereaza descriptorii TX terminati
void rtl8139_poll(int idx);
