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
- `make` (ISO în build/yokai.iso), `make run`, `make test` (smoke boot), `make debug` (GDB :1234), `make clean`.
- `ARCH=i386` acum; devine `x86_64` după boot-ul în long mode. Flaguri adiționale QEMU: `QEMU_EXTRA="..."`.
- Necesită pe Linux/WSL: gcc, g++, binutils, grub-pc-bin, xorriso, mtools, qemu-system-x86.
