# YokaiOS – comenzi

Prompt-ul shell-ului: `yokai@drone_terminal:~$`. Comenzile se potrivesc **exact** (fara alias-uri, fara pipe-uri, fara ghilimele, fara cai relative la un cwd – shell-ul nu are `cd`; folosește cai absolute). O linie goală nu face nimic; o comandă necunoscută afișează `command not found:`.

Backspace șterge ultimul caracter; linia are maxim 255 de caractere. Comenzile de fișiere (secțiunea 2) rulează într-un task separat (`shell-fs`); cât timp rulează, tastele sunt ignorate.

## 1. Comenzi shell – sistem

| Comandă | Ce face | Exemplu |
|---|---|---|
| `help` | Listează comenzile disponibile. | `help` |
| `clear` | Curăță ecranul. | `clear` |
| `version` | Afișează versiunea: `YOKAI OS version 0.1`. | `version` |
| `uptime` | Timpul de la pornire, în secunde cu milisecunde, plus jiffies. | `uptime` → `up 12.345 s (jiffies 12345)` |
| `date` | Ora reală UTC citită din RTC (corectată cu uptime-ul). | `date` |
| `lspci` | Câte o linie pentru fiecare dispozitiv PCI. | `lspci` |
| `lspci -v` | La fel, plus liniile BAR (adresă/dimensiune) ale fiecărui dispozitiv. Doar forma exactă `lspci -v`. | `lspci -v` |
| `lsblk` | Discurile și partițiile din HAL (sda, sda1, …), câte una pe linie. | `lsblk` |

## 2. Comenzi shell – fișiere (VFS)

Căile sunt absolute (`/etc/motd`). Comenzile sunt mute la succes; la eroare afișează o linie `comanda: cale: mesaj` (ex. `cat: /x: No such file or directory`).

| Comandă | Sintaxă | Ce face | Exemplu |
|---|---|---|---|
| `cat` | `cat <fisier>` | Afișează conținutul (maxim 4096 octeți; caracterele neafișabile devin `.`). | `cat /etc/hostname` → `yokai-drone` |
| `ls` | `ls [director]` | Listează un director (implicit `/`); directoarele au `/` la final; `.` și `..` sunt ascunse. | `ls /etc` |
| `echo` | `echo [text]` | Afișează textul pe ecran. | `echo salut` |
| `echo >` | `echo [text] > <fisier>` | Scrie textul + newline în fișier (îl creează sau îl suprascrie). | `echo ok > /tmp/x` |
| `echo >>` | `echo [text] >> <fisier>` | Adaugă la sfârșitul fișierului (îl creează dacă lipsește). | `echo inca >> /tmp/x` |
| `mkdir` | `mkdir <director>` | Creează un director (mod 0755). | `mkdir /tmp/d` |
| `rmdir` | `rmdir <director>` | Șterge un director gol. | `rmdir /tmp/d` |
| `rm` | `rm <fisier>` | Șterge un fișier. | `rm /tmp/x` |
| `mv` | `mv <sursa> <destinatie>` | Redenumește/mută (în același sistem de fișiere; altfel `Invalid cross-device link`). Căile nu pot conține spații. | `mv /tmp/a /tmp/b` |
| `sync` | `sync` | Scrie pe disc toate buffer-ele murdare (cache-ul de blocuri). | `sync` |
| `umount` | `umount <punct_de_montare>` | Demontează un sistem de fișiere (scrie metadatele, marchează volumul „curat”). | `umount /media` |

Note:
- Rădăcina `/` este initrd (tarfs) – **doar citire** (`Read-only file system` la scriere). Se poate scrie în `/tmp` și `/run` (tmpfs), iar pe `/media` (FAT32, primul disc FAT32 găsit) și pe un ext4 montat.
- FAT32: fără `umount /media` (sau măcar `sync`) volumul rămâne marcat „murdar” (`fsck.fat` îl raportează).
- Alte montări: `/dev` (devfs, noduri pentru dispozitive HAL), `/proc` (procfs).
- Fișiere din initrd: `/etc/hostname`, `/etc/motd`, `/etc/os-release`, `/etc/mtab`.

Mesaje de eroare posibile: `No such file or directory`, `Not a directory`, `Is a directory`, `File exists`, `Directory not empty`, `Read-only file system`, `No space left on device`, `File too large`, `Device or resource busy`, `Operation not permitted`, `Invalid argument`, `No such device`, `I/O error`; plus `missing operand` / `mv: missing destination` / `echo: missing file name`.

## 3. Programe în /bin (userspace)

Shell-ul **nu** lansează încă programe externe. Aceste programe sunt ELF-uri statice scrise în assembly (`user/test/*.S`), puse în `/bin` din initrd și rulate de testele kernelului:

| Program | Rol |
|---|---|
| `/bin/hello` | Verifică stiva inițială, segmentele și syscall-urile, apoi `exit(0)`. |
| `/bin/fault` | Provoacă intenționat o excepție în ring 3 (`argv[1][0]` alege tipul); kernelul trebuie să omoare doar procesul. |
| `/bin/forktest` | `fork`, pipe, `waitpid`, `execve`, `WNOHANG`, copil orfan. |
| `/bin/fstest` | Syscall-urile de fișiere din ring 3 (open/read/write/lseek/stat/mkdir/getdents64/mount/…). |

Un program nou: pune un `.S` în `user/test/` și va apărea în `/bin`.

## 4. Linia de comandă a kernelului (GRUB)

- `test=<nume>[,<nume>…]` – rulează suitele de teste: `cpu mm sched sync smp user proc rt hal pci mouse fb rtc usb drivers tarfs tmpfs devfs procfs block ahci fat32 ext4 ext4w vfs relock smash`.
- `root=/dev/sda2` – montează ca rădăcină partiția ext4 (vezi `make run-ext4`).
- `debug=mouse` / `debug=usb` – afișează pe serial evenimentele mouse / USB.
- `nofb` – consolă VGA text în loc de framebuffer.
- `noload` nu este argument de kernel, ci opțiune de montare ext4 (`vfs_mount(…, "noload")`): montează fără a reda jurnalul.
- `rt_strict` – cu `test=rt`, jitter strict.

## 5. Ținte `make`

Se rulează în WSL/Linux din rădăcina repo-ului. `SMP=n` setează numărul de CPU (implicit 4), `QEMU_EXTRA="…"` adaugă argumente QEMU.

| Țintă | Ce face |
|---|---|
| `make` / `make all` | Construiește kernelul, `initrd.tar` și `build/yokai.iso`. |
| `make run` | Pornește ISO-ul în QEMU (serial pe stdio). |
| `make debug` | Ca `run`, cu gdbstub (`-s -S`). |
| `make test` | Testul principal (`test=cpu,mm,sched,…,vfs`), comparat cu `tests/expect.txt`. |
| `make test-smash` | Trebuie să se termine cu panic „stack smashing”. |
| `make test-relock` | Trebuie să se termine cu panic „re-lock” (spinlock). |
| `make test-rt` | Jitter RT (KVM dacă `/dev/kvm` e scriabil, altfel `-icount`). |
| `make test-pci-q35` | PCI pe mașina q35 (ECAM/MCFG). |
| `make test-ahci` (`-gpt`, `-mbr`) | AHCI + partiții GPT / MBR. |
| `make test-fat32` (`test-fat32-1`, `-8`) | FAT32 cu clustere de 512 B / 4 KiB; necesită `dosfstools` + `mtools`. |
| `make test-ext4` | ext4 / ext2 / linear / refuse / recover (read-only). Necesită `e2fsprogs`. |
| `make test-ext4-refuse`, `test-ext4-recover` | Cazurile individuale. |
| `make test-ext4w` (`test-ext4w-<caz>`) | Scriere ext4 + jurnal JBD2, cu căderi simulate; cazuri: `crash1..crash5 clean kill replay replay-csum`. |
| `make test-ext4-host` | Același cod ext4/jbd2 pe gazdă cu ASAN/UBSan, sute de căderi simulate. |
| `make run-ext4` | Boot normal cu rădăcina ext4 pe `sda2`. |
| `make test-input-ps2`, `test-input-usb` | Tastatura prin PS/2, respectiv USB. |
| `make test-mouse`, `test-usb` | Mouse / xHCI. |
| `make test-netutils` | ping, ifconfig, netstat, wget, nc (T9.5): `/bin/nettest` + `tests/netutils_host.py` (server HTTP/echo pe gazdă, hostfwd spre `nc -l 7`). |
| `make test-nofb` | Consolă VGA text. |
| `make clean` | Șterge `build/`. |
