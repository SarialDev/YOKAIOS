#pragma once

// Disc SATA real in QEMU (make test-ahci-gpt / test-ahci-mbr): sectorul 0, partitiile din HAL, DMA multi-segment,
// intreruperi MSI, buffer cache; cmdline "test=ahci". Fara disc AHCI se opreste cu panic
void test_ahci_run();
