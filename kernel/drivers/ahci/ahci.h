#pragma once
#include "types.h"
#include "hal/block.h"

// Driver AHCI (PCI clasa 01:06, prog-if 01): reset HBA, pornire porturi, IDENTIFY, READ/WRITE DMA EXT si
// FLUSH CACHE EXT pe slotul 0 (fara NCQ), MSI cand exista, altfel interogare. Discurile SATA apar in HAL ca
// sda, sdb... si partitiile lor ca sda1... (block/partition.h). ATAPI si multiplicatoarele de porturi se ignora.

// Inregistreaza driverul PCI; probe-ul ruleaza imediat pe dispozitivele deja enumerate
void ahci_init();
int ahci_controller_count();
int ahci_disk_count();
BlockDevice* ahci_disk(int idx);
// Intreruperi MSI tratate de la boot (pentru teste)
uint32_t ahci_irq_count();
