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

## Framebuffer / consola grafică
- `boot.S` cere 1024x768x32 prin tag-ul Multiboot2 (opțional); GRUB încarcă `all_video`. `kernel/drivers/fb/fb.{h,cpp}`: mapare WC (PAT: intrarea 1 = WC, deci doar `PTE_PWT`; `pat_init_cpu()` pe fiecare CPU) la `HHDM_BASE + 256 GiB + fizic`, `putpixel/fill_rect/blit/draw_glyph/scroll_up/flush`, back buffer în RAM (write-through: se scrie și în fb; scroll citește doar din RAM). Device HAL `fb0`.
- `kernel/drivers/fbcon.cpp`: consolă text 8x16 (font Terminus în `fb/font8x16.h`, Latin-15; 0xDB/0xDC/0xDF = blocuri CP437 sintetizate), ANSI (SGR 0/1/7/22/30-37/39/40-47/49/90-97/100-107, `H f A B C D J K`), temă Dark (alb pe negru, accente roșii), cursor = linie roșie. Se înregistrează ca „fbcon” și VGA text tace (`vga_disable`). Fără fb, sau cu cmdline `nofb`, rămâne VGA text.
- Mesajele de boot de dinainte de `kmalloc_init` merg doar pe serial. Teste: `test=fb`, `make test-nofb` (expect.txt: prefixele `@fb ` / `@nofb `, variabila `FB=0`).

## VFS
- `kernel/fs/vfs/`: `Inode`+`InodeOps` (lookup/create/mkdir/symlink/unlink/rmdir/read/write/truncate/readdir/readlink, read/write pe offset), `SuperBlock`+`SuperBlockOps`, `FileSystemType` (`fs_register`/`fs_find`), `Dentry` (cache cu hash, negative dentry doar tranzitoriu, copiii tin referinta la parinte), `Mount` (arbore de mount-uri, mount stivuit), `Path{mnt,dentry}`, `File` (proc/file.h: inode, dentry, pos, flags; `vfs_file_ops`).
- Un singur lock (`vfs_mutex`) serializeaza namespace-ul si contoarele de referinte; functiile `vfs_*` din `vfs.h` il iau singure. Read/write pe date nu il iau (le protejeaza fs-ul). Primul mount trebuie sa fie pe "/".
- `path_lookup`: absolute/relative, ".", "..", slash-uri multiple/final, mount point-uri (si ".." peste ele), symlink-uri (max 8 urmate per rezolvare, apoi -ELOOP). `Process::cwd` (dentry nullptr = radacina), mostenit la fork.
- Syscall-uri (numerele Linux): open, close, read, write, lseek, stat/lstat/fstat, mkdir, rmdir, unlink, symlink, readlink, getdents64, chdir, getcwd, mount, umount2 in `kernel/syscall/fs.cpp`. `execve` citeste ELF-ul prin VFS (`exec_replace`/`exec_spawn` in `kernel/exec/elf.cpp`, max 4 MiB, relativ la `Process::cwd`).
- Fara permisiuni (T8.1) si fara page cache (T6.5). Niciun fs real inca: `test=vfs` foloseste un memfs fals din `kernel/tests/test_vfs.cpp` si programul user `user/test/fstest.S`.

## initrd / tarfs
- `make` construieste `build/initrd.tar` (ustar): `rootfs/` (continut real, ex. `etc/hostname`) + directoarele din blueprint (`ROOT_DIRS` din Makefile) + programele user din `build/user/*.elf` copiate in `/bin` fara `.elf` + symlink `etc/mtab`. GRUB il incarca ca modul `initrd` (`module2 /boot/initrd.tar initrd`, boot/grub*.cfg).
- `kernel/fs/tarfs.{h,cpp}`: FS `tarfs`, read-only (scrierile intorc `-EROFS`, `MS_RDONLY` pe sb). Arhiva se parseaza la mount intr-un arbore de noduri (fisierele si tintele symlink-urilor arata direct in memoria modulului). Suporta fisiere, directoare (si implicite), symlink, hardlink, prefix ustar; ignora dispozitive/pax/GNU long names (tar cu `--format=ustar`). Montat pe `/` la boot (`tarfs_mount_root`, dev_name = numele modulului).
- Shell: `cat <cale>` si `ls [cale]` (VFS-ul foloseste mutex, deci ruleaza in task-ul `shell-fs`, nu in IRQ; tastele sunt ignorate pana termina). Testele tasteaza `cat /etc/hostname` (tests/run_qemu.sh).
- Teste: `test=tarfs` (`kernel/tests/test_tarfs.cpp`); `test_vfs` desprinde radacina initrd cu `umount_root_do()` (VFS gol pentru memfs) si o pune la loc la final; fstest.elf vine dintr-un tarfs montat pe `/mnt`.

## tmpfs / devfs
- `kernel/fs/tmpfs.cpp`: fisiere in pagini 4 KiB alocate la cerere (tabela nivel 1 -> frunze -> pagini, max 1 GiB/fisier, goluri = zerouri fara memorie), directoare, symlink-uri. Un `Mutex` pe nod protejeaza datele (read/write nu iau lock-ul VFS). Optiuni mount (`data`): `size=<KiB>` (implicit jumatate din RAM, -ENOSPC la depasire), `mode=<octal>`. Montat pe `/tmp` (mode=1777) si `/run` (mode=0755).
- `kernel/fs/devfs.cpp`: nu stocheaza nimic, nodurile se deriva din HAL la lookup/readdir (fara udev, create/mkdir/unlink = -EPERM). `null`, `zero`, `random`/`urandom` (RDRAND, fallback TSC necriptografic), `console` (scrie pe toate consolele), `ttyS0` (consola "serial"), `fb0` (pixeli de 32 bit, linii fara padding, prin `fb_putpixel`), plus fiecare `BlockDevice` HAL sub numele inregistrat de driver: driverele de disc trebuie sa faca `hal_register(HalKind::Block, dev, "sda")` / `"nvme0n1"`. Block = acces la octet (sectoarele partiale: citeste-modifica-scrie). read pe console/ttyS0 da EOF (nu exista tty inca).
- `kernel/fs/pseudofs.{h,cpp}`: `tmpfs_init/devfs_init` (inregistrare), `fs_mount_pseudo()` (dupa initrd, in main) si `fs_umount_pseudo()` (testele care cer VFS gol, ex. test_vfs).
- Shell: `echo [text] [> | >> fisier]` (in task-ul `shell-fs`); `echo tmpfsok > /tmp/x` apoi `cat /tmp/x`. Testele: `test=tmpfs`, `test=devfs`; `run_qemu.sh` tasteaza echo/cat pe /tmp si cere `tmpfsok`.
