# YokaiOS

OS hobby hibrid: x86-64 acum, ARM64 (dronă) mai târziu.

## Structură
- `boot/` – cod de boot în asm și linker script
- `kernel/` – sursele kernelului
- `tests/` – teste în QEMU
- `docs/` – blueprint și plan (vin mai târziu)

## Lucru
- Un subtask `Tx.y` per branch: `feature/Tx.y-nume`.
- Build: `make`. Rulare: `make run`.
- Comentarii minime.
- Răspunsurile finale: maxim 5 rânduri.

## Build
- `make` (ISO în build/yokai.iso), `make run`, `make test` (QEMU headless, verifică markerii din tests/expect.txt, log în build/test.log), `make debug` (GDB :1234), `make clean`.
- Kernel x86-64 higher-half (0xFFFFFFFF80000000), boot Multiboot2. Flaguri adiționale QEMU: `QEMU_EXTRA="..."`. CPU-uri: `SMP=n` (implicit 4; `make test` cere `[OK] smp cpus=4`).
- Necesită pe Linux/WSL: gcc, g++, binutils, grub-pc-bin, xorriso, mtools, qemu-system-x86.

## HAL
- `kernel/hal/`: interfețe abstracte (Console, ClockSource, InterruptController, BlockDevice, NetDevice, Framebuffer, InputDevice) + registru `hal_register/hal_get/hal_list`. Ultimul dispozitiv înregistrat per tip e „primary” (APIC peste PIC).
- Kernelul folosește hardware doar prin HAL: `kprintf` -> `hal_console_putc`, EOI/IPI/timer local -> `hal_irqchip()`, ceas -> `hal_clock()`. Driverele (serial, vga, ps2kbd, tsc, irqchip.cpp) se înregistrează la boot.
- Input: event bus comun (ring buffer) în `hal/input.h`; `input_push` din IRQ, `input_pop`/`input_subscribe` pentru consumatori.
- Obiectele HAL sunt statice cu constructor `constexpr` (nu rulează constructori globali în kernel).


## PCI
- `kernel/drivers/pci/pci.{h,cpp}`: ECAM din MCFG (fallback porturi 0xCF8/0xCFC), scanare recursivă prin bridge-uri, `PciDevice` cu BAR-uri decodate, `pci_enable`, MSI/MSI-X (`pci_msi_enable`, `pci_msix_*`, vectori 0x90-0xEF cu EOI în dispatcher), tabel de drivere `PciDriver` + `pci_register_driver`. Shell: `lspci`, `lspci -v`. Fără hotplug.
- Testele rulează cu `-nic e1000`, `-device edu` (MSI) și `virtio-net-pci` (MSI-X) puse de `tests/run_qemu.sh`; `make test-pci-q35` verifică ECAM.
