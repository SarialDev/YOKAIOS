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

