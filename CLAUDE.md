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
- `kernel/fs/vfs/`: `Inode`+`InodeOps` (lookup/create/mkdir/symlink/unlink/rmdir/read/write/truncate/readdir/readlink/rename, read/write pe offset), `SuperBlock`+`SuperBlockOps`, `FileSystemType` (`fs_register`/`fs_find`), `Dentry` (cache cu hash, negative dentry doar tranzitoriu, copiii tin referinta la parinte), `Mount` (arbore de mount-uri, mount stivuit), `Path{mnt,dentry}`, `File` (proc/file.h: inode, dentry, pos, flags; `vfs_file_ops`).
- Un singur lock (`vfs_mutex`) serializeaza namespace-ul si contoarele de referinte; functiile `vfs_*` din `vfs.h` il iau singure. Read/write pe date nu il iau (le protejeaza fs-ul). Primul mount trebuie sa fie pe "/".
- `path_lookup`: absolute/relative, ".", "..", slash-uri multiple/final, mount point-uri (si ".." peste ele), symlink-uri (max 8 urmate per rezolvare, apoi -ELOOP). `Process::cwd` (dentry nullptr = radacina), mostenit la fork.
- Syscall-uri (numerele Linux): open, close, read, write, lseek, stat/lstat/fstat, mkdir, rmdir, unlink, rename (82), symlink, readlink, getdents64, chdir, getcwd, mount, umount2 in `kernel/syscall/fs.cpp`. `execve` citeste ELF-ul prin VFS (`exec_replace`/`exec_spawn` in `kernel/exec/elf.cpp`, max 4 MiB, relativ la `Process::cwd`).
- `vfs_rename` (ca rename(2): fisier peste fisier / director gol peste director gol, -EXDEV intre sb, -EINVAL in propriul subarbore, -EPERM fara `InodeOps::rename`); dupa succes dentry-urile numelui vechi si nou se scot din cache (`d_drop`). tmpfs si FAT32 stiu rename; tarfs/devfs/procfs nu.
- Fara permisiuni (T8.1). `test=vfs` foloseste un memfs fals din `kernel/tests/test_vfs.cpp` si programul user `user/test/fstest.S`.

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

## procfs
- `kernel/fs/procfs.cpp`, FS `proc`, montat pe `/proc` de `fs_mount_pseudo()` (read-only, `SB_NOCACHE`: dentry-urile si inode-urile dispar la ultima referinta, deci procesele terminate dispar din `/proc`). Continutul se genereaza la fiecare `read` intr-un buffer de kernel de 8 KiB (size = 0 la stat, ca pe Linux); nu exista sysctl.
- `/proc/meminfo` (MemTotal/MemFree/MemAvailable/Buffers/Cached/Swap*, ca formatul Linux: nume pana la col. 15, valoare pe 8 coloane, ` kB`), `/proc/uptime` (timpul idle nu se contorizeaza: 0.00), `/proc/cpuinfo` (CPUID; aceleasi valori pentru toate CPU-urile), `/proc/mounts` (`dev punct tip ro|rw 0 0`; `etc/mtab` e symlink aici), `/proc/self` (symlink la pid-ul celui care citeste).
- `/proc/<pid>/{status,cmdline,maps}` pentru fiecare task (fara idle): pid = tid; `status` (Name/State/Tgid/Pid/PPid/Threads/VmSize/VmRSS/Cpu/SchedClass/Prio), `cmdline` (argv separat prin `\0`, `Process::cmdline`, setat la `exec_spawn`/`exec_replace`, copiat la fork), `maps` (regiuni user citite din tabelele de pagini, ca `r-xp`/`rw-p`, `[stack]`; fara fisiere/offset-uri). Thread-urile de kernel au cmdline/maps goale. Un proces ZOMBIE ale carui task a fost eliberat nu mai apare.
- Suport: `task_info/task_nth` (sched/task.cpp), `mount_snapshot` (+ `Mount::dev_name`), `clock_tsc_hz`. Teste: `test=procfs`; `run_qemu.sh` tasteaza si `cat /proc/meminfo` (expect: `MemTotal:`).

## AHCI / block layer
- `kernel/block/blockdev.{h,cpp}`: `BlockRequest` + `BlockQueue` (coada FIFO de cereri, un request activ; `submit_wait` asteapta prin `Completion` cand MSI e activ si se ruleaza intr-un task obisnuit, altfel interogheaza hardware-ul cu `poll`; task-ul idle/boot nu se poate bloca, deci ramane pe interogare; timeout 5 s -> `abort` + `-EIO`). `block_read_bytes/block_write_bytes` (octeti peste sectoare), `block_format`/`block_dump` (shell `lsblk`).
- `kernel/block/bcache.{h,cpp}`: `BufferCache` cu blocuri de 4 KiB, hash + LRU, write-back: `read`/`get` (pinned), `mark_dirty`, `release`, `sync(dev)` (scrie murdarele + `flush()` pe dispozitiv), `invalidate`, `writeback_aged`. Un `Mutex` global serializeaza structura (si I/O-ul facut sub el). Global: `bread/bget/bdirty/brelse/bsync`, task `bflush` (la 1 s scrie blocurile murdare mai vechi de 5 s). Nu e coerent intre disc si partitiile lui: un fs foloseste ori `sda`, ori `sda1`. devfs (`/dev/sda`) ocoleste cache-ul.
- `kernel/block/partition.{h,cpp}`: `partition_scan` (MBR primare 1-4 + logice 5+ prin EBR, GPT cu CRC32 pe antet si tabela, antet de rezerva), `PartitionDevice` (offset + limite, `flush` merge la parinte), `partition_register(disk, "sda")` -> `hal_register` pentru `sda1`.. (`nvme0n1p1` daca numele se termina in cifra). `HAL_MAX_PER_KIND` = 16.
- `kernel/drivers/ahci/ahci.{h,cpp}`: driver PCI (clasa 01:06, prog-if 01), pana la 2 HBA si 8 discuri (`sda`..). HBA reset, per port: stop, CLB/FIS intr-o pagina, FRE, semnatura (doar ATA 0x101; ATAPI ignorat), ST, IDENTIFY (LBA48 obligatoriu, sector logic 512..4096), READ/WRITE DMA EXT si FLUSH CACHE EXT pe slotul 0 (fara NCQ), PRDT scatter/gather peste paginile buffer-ului (adresa para, max 128 KiB per cerere, `vmm_translate` in task, nu in IRQ). MSI (altfel doar interogare; INTx nu e suportat). Eroare/timeout -> stop/start port, COMRESET, altfel disc "mort".
- Teste: `test=block` (in `make test`, disc in memorie: cache, LRU, write-back, MBR cu logice, GPT, antet de rezerva); `make test-ahci-gpt` / `test-ahci-mbr` (`tests/mkdisk.py` face imaginea de 32 MiB; QEMU `-boot order=d -device ahci -device ide-hd`; `test=block,ahci`: sectorul 0, partitii, DMA pe poll si pe MSI intr-un task, bcache pe disc real). QEMU cu disc SATA trebuie pornit cu `-boot order=d`, altfel SeaBIOS booteaza discul.

## FAT32
- `kernel/fs/fat32/`: `fat.cpp` (volum peste `bread/bget/bdirty` pe dispozitivul partitiei, **cache de FAT** = 16 ferestre de 4 KiB in memorie cu LRU + scriere in toate copiile FAT prin bcache, alocare cu `next_free`, `free_count`, lanturi), `dir.cpp` (intrari de director, LFN citire/scriere cu checksum, nume 8.3 cu biti NT, aliasuri `~N`, UTF-8<->UTF-16, comparatie fara majuscule inclusiv romanesti, timp FAT<->epoch), `fat32.cpp` (tip `fat32`, InodeOps/SuperBlockOps, mount, automount). Header public `fat32.h`.
- Mount: sursa = nume HAL ("sda1"), optiune "ro"; BPB (bps 512..4096, FAT32 strict: root_entries=0, fat16=0), FSInfo (recalculat daca lipseste/e invalid), FAT oglindit sau doar FAT-ul activ. `fat32_automount()` (main, dupa fs_mount_pseudo) monteaza primul volum FAT32 din HAL pe `/media`. Un volum se monteaza pe partitie SAU disc, niciodata ambele (bcache nu e coerent).
- Un `Mutex` per volum serializeaza tot; sb are `SB_NOCACHE` (dentry-urile nu raman in cache -> nu exista nume vechi dupa unlink/rename; lookup-ul scaneaza directorul, case-insensitive, si dupa alias 8.3). `Node` (inode viu) e legat de intrarea scurta: (cluster director parinte, offset); inode-ul se elibereaza la ultima referinta (la nlink 0 si lantul de clustere). ino = primul cluster (sintetic pentru fisiere goale).
- Scriere: prima modificare marcheaza volumul murdar (octet 0x41 din boot sector + FAT[1] bit 27); `umount` (`fat_kill`) scrie FSInfo, readuce bitii "curat" si face `invalidate` pe dispozitiv. **Fara umount ramane murdar** (fsck.fat o raporteaza); in shell: `sync`, `umount /media`. Dimensiune maxima 4 GiB-1 (-EFBIG), fara symlink, fara hard link, permisiuni fixe (0644/0755, 0444 daca ATTR_RO), uid/gid 0.
- Extindere/trunchiere: clustere noi zerorizate la alocare, coada ultimului cluster se zeroizeaza cand fisierul creste (scriere dupa EOF, truncate); trunchierea elibereaza lantul de dupa `ceil(size/cluster)`. Director: creste cu clustere zerorizate (max 2 MiB), intrarile sterse (0xE5) se refolosesc. mkdir scrie `.`/`..` (`..` = 0 cand parintele e radacina); rename muta si `..`.
- Shell: `mkdir`, `rmdir`, `rm`, `mv`, `sync`, `umount` (task `shell-fs`), pe langa `ls`/`cat`/`echo >`.
- Teste: `make test-fat32` (= `-1` si `-8`: clustere de 512 B / 4 KiB). `tests/mkfat32.sh` face un disc de 64 MiB (MBR, sda1 FAT32 cu `mkfs.fat`, populat cu `mtools`: 8.3, LFN, directoare, big.bin de 300 KB); kernelul ruleaza `test=fat32` (`kernel/tests/test_fat32.cpp`: citire, scriere/trunchiere, nume si Unicode, director mare, rename/rmdir/unlink, 2 MiB, volum plin cu ENOSPC, remount, remount "ro"); apoi `tests/check_fat32.sh` ruleaza `fsck.fat -n` pe imagine (trebuie curata) si verifica cu mtools ce a lasat kernelul. Necesita pe gazda: dosfstools + mtools (CI: `apt install dosfstools mtools`). `test=tmpfs` acopera si rename.

## ext4 (read-only)
- `kernel/fs/ext4/`: `inode.cpp` (citire prin bcache, `map_block`: arbore de extente pana la 5 niveluri, extente neinitializate = zerouri, sau harta de blocuri 12+indirect simplu/dublu/triplu; goluri = zerouri; `node_read`), `dir.cpp` (intrari liniare, **htree**: hash legacy/half_md4/tea + variantele "unsigned", seed din superblock, descent pe 1-3 niveluri, continuare in frunza urmatoare la coliziuni; cade pe scanare liniara daca indexul are structura neasteptata), `ext4.cpp` (montare, VFS, symlink-uri rapide/lente). Header public `ext4.h`.
- Montare: tip `ext4`, sursa = nume HAL (`sda2`, `/dev/` se ignora), optiuni `ro`, `noload`; **intotdeauna read-only** (`MS_RDONLY`, scrierile intorc -EROFS; scrierea e T6.8). Superblock + descriptori de grup 32/64 biti (si `meta_bg`, `sparse_super`/`sparse_super2`) se citesc la mount (doar `inode_table` per grup). Nu se verifica checksum-urile (metadata_csum e acceptat, ignorat).
- Feature flags: INCOMPAT permise = filetype, extents, 64bit, flex_bg, meta_bg, mmp, ea_inode, csum_seed, largedir, recover; **orice alt bit INCOMPAT (compresie, journal_dev, dirdata, inline_data, encrypt, casefold, necunoscut) => -EINVAL**, la fel `bigalloc` (RO_COMPAT) si blocuri > 4 KiB. `needs_recovery` (jurnal nereluat) refuzat fara `noload`; jurnalul nu se reda niciodata.
- Inode-urile se incarca o data si raman in memorie pana la umount (hash 64 de cosuri, lock per volum la lookup/readdir; read nu ia lock). Fara xattr/ACL, fara permisiuni (T8.1); timpi cu extra epoch/nanosecunde; uid/gid pe 32 biti.
- **Radacina**: cmdline `root=/dev/sda2` (sau `root=sda2`) => `ext4_mount_root()` in `main.cpp` inainte de `fs_mount_pseudo()` (imaginea trebuie sa aiba `/dev /proc /tmp /run /mnt /bin /etc`); daca montarea esueaza sau lipseste root=, radacina e initrd-ul (tarfs) ca inainte. `execve` citeste prin VFS, deci `/bin/*` de pe ext4 functioneaza.
- Teste: `make test-ext4` = `test-ext4-ext4` (4 KiB, extente, 64bit, metadata_csum, htree 1 nivel), `-ext2` (1 KiB, harta de blocuri, htree 2 niveluri, indirectare tripla), `-linear` (2 KiB, meta_bg cu 56 grupuri, fara dir_index), `-refuse` (flag INCOMPAT necunoscut), `-recover` (needs_recovery). `tests/mkext4.sh` face imaginea cu `mke2fs -d` (rootfs + /data cu fisiere la pragurile de blocuri, sparse de 66 MiB, 1500 insule = arbore de extente adanc, symlink-uri, hard link, fifo, /many 3000 si /huge 9000 intrari, nume de 255, Unicode) + `e2fsck -fyD` pentru index htree; kernelul (`test=ext4 ext4mode=ok|refuse|recover`, `kernel/tests/test_ext4.cpp`) verifica si apoi tipareste cate o linie `EXT4 F <crc32> <dim> <mod> <cale>` pentru tot arborele; `tests/check_ext4.sh` ruleaza `e2fsck -fn` si compara liniile cu `tests/ext4_list.py` (hash identic cu sursa). Necesita e2fsprogs pe gazda (CI: `apt install e2fsprogs`). `make run-ext4` booteaza GUI cu radacina pe sda2.
