# Formatul pachetelor `.yokai` (versiunea 1)

Un pachet `.yokai` este o arhivă **tar ustar** necomprimată. Prima intrare este `manifest.toml`; restul sunt fișierele
(și directoarele) pachetului, cu calea lor **relativă la rădăcina sistemului** (`opt/hello/bin/hello` se instalează ca
`/opt/hello/bin/hello`). Pachetele se construiesc pe gazdă cu `tools/mkyokai` și se instalează în YokaiOS cu `/bin/ypkg`.

```
manifest.toml
opt/hello/bin/hello
opt/hello/share/hello.txt
```

## manifest.toml

Subset de TOML: șiruri (`"..."` cu `\" \\ \n \t \r`, sau `'...'` literale), întregi (zecimal, `0o` octal, `0x` hex), liste de
șiruri, tabele inline și tabelul `[files]`. Comentariile încep cu `#`. Chei necunoscute la nivel superior se ignoră (formate
viitoare compatibile); `[[tablouri]]` și alte secțiuni decât `[files]` sunt refuzate.

```toml
format = 1                                  # obligatoriu; ypkg refuză alte valori
name = "hello"                              # obligatoriu: [a-z0-9][a-z0-9._+-]{0,63}
version = "1.0.0"                           # obligatoriu: [A-Za-z0-9][A-Za-z0-9.+~-]{0,63}
description = "Program de proba"            # opțional
entry = "/opt/hello/bin/hello"              # opțional: fișierul principal (trebuie să fie în [files]); `ypkg run`
deps = ["libfoo >= 1.2", "bar"]             # opțional: "nume [op versiune]", op ∈ >= <= > < = ==
permissions = ["tty", "cap.net"]            # opțional: etichete libere (se afișează); cele "cap.<nume>" sunt plafonul de capabilități, vezi mai jos
signature = ""                              # obligatoriu ca cheie; GOL până la T8.5

[files]
"opt/hello/bin/hello"       = { sha256 = "7fe18d6b…0ce4", size = 59760, mode = 0o755 }
"opt/hello/share/hello.txt" = { sha256 = "cbd23200…d437", size = 44,    mode = 0o644 }
```

* `[files]` conține **fiecare fișier obișnuit** din arhivă: calea (cheie între ghilimele), `sha256` (64 cifre hex
  minuscule, peste conținut), `size` (octeți) și `mode` (permisiuni, `0o000`..`0o777`, implicit `0o644`). Permisiunile
  din antetul tar se ignoră; contează cele din manifest.
* Directoarele nu apar în `[files]`; intrările de director din tar se creează la instalare. Directoarele părinte ale unui
  fișier se creează oricum.
* Versiunile se compară pe componente separate prin `. + ~ -`: componentele numerice ca numere (`1.10 > 1.9`), celelalte
  ca text, o componentă lipsă = 0 (`1 == 1.0.0`).

## Câmpul `signature` (T8.5)

Acum rămâne gol (`signature = ""`); ypkg îl afișează în `info` (`(nesemnat)` dacă e gol) dar nu îl verifică. T8.5 îl va
umple: semnătura va acoperi `manifest.toml` cu linia `signature = ...` înlocuită prin `signature = ""`, iar manifestul
acoperă conținutul prin hash-urile din `[files]`, deci o semnătură pe manifest protejează tot pachetul.

## Reguli de validare (ypkg)

ypkg verifică **tot** înainte să scrie ceva (și verifică din nou hash-urile în timp ce extrage):

1. prima intrare este `manifest.toml` (cel mult 1 MiB) și se parsează fără erori; `format = 1`, nume/versiune valide,
   `signature` prezent, dependențele bine formate, `entry` (dacă există) este un fișier din `[files]`;
2. fiecare intrare tar este fișier obișnuit sau director — legăturile simbolice/hard și dispozitivele sunt refuzate;
3. fiecare cale este relativă, fără componente goale, `.` sau `..`, fără caractere de control, și începe cu unul din
   `bin sbin lib usr etc opt var home`; `var/lib/ypkg` (baza de date) este interzis;
4. fiecare fișier din arhivă este în `[files]`, o singură dată, cu `size` și SHA-256 exacte; nimic din `[files]` nu lipsește;
5. pachetul nu este deja instalat și dependențele sunt instalate cu versiunea cerută;
6. niciun fișier din `[files]` nu există deja pe disc (conflict) — ypkg nu suprascrie.

Dacă extragerea eșuează la mijloc (disc plin, fișier blocat), tot ce s-a creat se șterge.

## Capabilități la exec (`cap.*`)

Permisiunile care încep cu `cap.` nu sunt simple etichete: sunt **plafonul de capabilități** (kernel/proc/caps.h) al
executabilelor pachetului. Nume: `cap.sys_admin`, `cap.net`, `cap.rawio`, `cap.rt`, `cap.mount`, `cap.kill`,
`cap.setuid`. La instalare, `ypkg` scrie în `/var/lib/ypkg/exec.caps` câte o linie `/<cale> <cap,cap|none> <pachet>` pentru
fiecare fișier cu cel puțin un bit `x` din `[files]`; la fiecare `execve` kernelul canonicalizează fișierul executat
(symlink-urile nu ocolesc), îl caută acolo (și în `/etc/exec.caps`) și taie capabilitățile procesului la acel plafon.
Un pachet fără nicio permisiune `cap.*` își pierde **toate** capabilitățile (`none`); `ypkg remove` șterge liniile.
Capabilitățile nu cresc niciodată la exec. `ypkg install` refuză un pachet cu `cap.<nume>` necunoscut sau cu un executabil
a cărui cale conține spațiu sau `#`.

## Baza de date

`<rădăcină>/var/lib/ypkg/<nume>/`:

* `files` — o linie per element creat: `F cale` (fișier) sau `D cale` (director creat de pachet), căi relative la rădăcină;
* `manifest.toml` — copia exactă a manifestului; **se scrie ultimul** și un pachet contează ca instalat doar dacă există.

`remove` șterge fișierele din `files`, apoi directoarele create (în ordine inversă; rămân dacă mai au conținut) și intrarea.
Refuză dacă un alt pachet instalat depinde de cel scos, în afară de `-f`.

## ypkg

```
ypkg [-r rădăcină] [-f] install FIȘIER.yokai   verifică, extrage, înregistrează
ypkg [-r rădăcină] [-f] remove NUME            -f ignoră dependenții inverși
ypkg list                                      nume, versiune, descriere (sortat)
ypkg info NUME | FIȘIER.yokai                  manifestul (+ lista de fișiere cu mărime și mod)
ypkg run NUME [arg...]                         execută `entry`
ypkg verify FIȘIER.yokai                       doar verificările 1-4, fără instalare
```

Rădăcina implicită e `/` (sau `$YPKG_ROOT`). Coduri de ieșire: 0 ok, 1 eroare de operație/validare, 2 utilizare.

`/` este initrd (read-only), de aceea `/sbin/init` montează `tmpfs` pe `/var` și `/opt` când rădăcina e read-only: pachetele
instalate și baza de date se pierd la repornire. Cu rădăcina pe un disc scriibil (`root=/dev/sda2`) instalarea e permanentă.
Pachetele de probă (`hello`, `hello-extra`) sunt în initrd, în `/usr/share/ypkg/`.

## mkyokai (gazdă)

```
tools/mkyokai build DIRECTOR [-o iesire.yokai]   DIRECTOR/manifest.toml = șablonul (fără [files]) + DIRECTOR/root/ = conținutul
tools/mkyokai verify FIȘIER.yokai
tools/mkyokai info FIȘIER.yokai
```

Calculează SHA-256, mărimea și modul (`0o755` dacă fișierul are vreun bit de execuție, altfel `0o644`), adaugă `[files]` și
`signature = ""` dacă lipsește, și scrie un tar ustar determinist (ordine alfabetică, mtime = `$SOURCE_DATE_EPOCH` sau 0,
uid/gid 0). Același director dă aceiași octeți. Cere doar Python 3 (cu `tomllib`, la 3.11+, validează sintaxa șablonului).

În repo, `make` construiește `build/pkg/<nume>.yokai` din `user/pkg/<nume>/` (`manifest.toml`, `root/`, iar fiecare `*.c` se
compilează în `root/opt/<nume>/bin/<nume>`) și le pune în initrd. `make test-ypkg` rulează `tests/ypkgtest.c`.

## Neacoperit (acum)

Fără compresie, fără upgrade (`install` peste un pachet existent e refuzat; `remove` și `install` din nou), fără scripturi
de instalare, fără verificarea semnăturii (T8.5), fără repo de pachete în rețea (după T9), fără `conflicts`/`provides`.
