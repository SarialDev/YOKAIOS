#pragma once
#include "types.h"
#include "hal/net.h"

// Driver Intel 82540EM (PCI 8086:100e, "e1000" din QEMU): reset, MAC din EEPROM (altfel RAL0/RAH0), inele RX/TX de
// 256 descriptori cu buffere de 2 KiB (memorie DMA sub 4 GiB, alocata contiguu din pmm), MSI daca dispozitivul are
// capabilitatea, altfel interogare (QEMU nu da MSI pe e1000: cadrele se iau la recv() / stats() / din task-ul de
// interogare "e1000p", creat la primul set_rx_handler). Fara offload TSO/checksum, fara VLAN, un singur
// buffer pe cadru (MTU 1500).
// Fiecare placa apare in HAL (HalKind::Net) ca eth0, eth1; pana la 2 placi.

// Inregistreaza driverul PCI; probe-ul ruleaza imediat pe placile deja enumerate
void e1000_init();
int e1000_count();
NetDevice* e1000_dev(int idx);
// 0 = interogare, 1 = MSI
int e1000_irq_mode(int idx);
// Ia cadrele sosite (numara, apeleaza handlerul); ce face si intreruperea
void e1000_poll(int idx);
