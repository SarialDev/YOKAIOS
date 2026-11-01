#pragma once
#include "types.h"
#include "hal/block.h"

// Driver NVMe (PCI clasa 01:08, prog-if 02): reset controller, coada de administrare, IDENTIFY (controller, lista de
// namespace-uri active, namespace), o pereche de cozi I/O (qid 1), READ / WRITE / FLUSH cu PRP (lista PRP cand
// transferul trece de doua pagini), MSI-X (intrarea 0 admin, intrarea 1 I/O), altfel MSI, altfel interogare.
// Fiecare namespace utilizabil (fara metadate, sector 512..4096) apare in HAL ca nvme<ctrl>n<nsid> si partitiile lui
// ca nvme0n1p1... (block/partition.h). Un singur request I/O in zbor (BlockQueue), pana la 128 KiB per request.

// Inregistreaza driverul PCI; probe-ul ruleaza imediat pe dispozitivele deja enumerate
void nvme_init();
int nvme_controller_count();
int nvme_ns_count();
BlockDevice* nvme_ns(int idx);
// Intreruperi tratate de la boot pe coada I/O (pentru teste)
uint32_t nvme_irq_count();
// 0 = interogare, 1 = MSI, 2 = MSI-X pentru controllerul idx
int nvme_irq_mode(int ctrl);
