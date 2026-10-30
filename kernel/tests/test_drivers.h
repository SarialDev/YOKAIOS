#pragma once

// Teste de integrare ale driverelor: lspci contine e1000 (8086:100e), framebuffer 1024x768 (fara "nofb"),
// intrare reala: asteapta ca harness-ul (tests/run_qemu.sh) sa trimita "version<ret>" prin monitorul QEMU;
// cmdline "test=drivers"
void test_drivers_run();
