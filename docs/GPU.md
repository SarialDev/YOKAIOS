# Accelerare GPU in YokaiOS

Stare: **T10.6 — virtio-gpu 2D ca backend al compozitorului** (facut). Vulkan complet ramane obiectiv pe termen lung; mai jos sunt pasii.

## Ce exista acum

### Kernel: `kernel/drivers/gpu/virtio_gpu.{h,cpp}`

- Driver PCI pentru `1af4:1050` (QEMU `-device virtio-gpu-pci` si `-device virtio-vga`). Transport virtio 1.x „modern”: capabilitatile vendor (0x09) din config space dau `common_cfg`, `notify_cfg` si `device_cfg` in BAR-uri mapate cu `pci_map_bar`.
- Negociaza doar `VIRTIO_F_VERSION_1`. Bitii functiilor GPU (`VIRGL`, `EDID`, `RESOURCE_UUID`, `RESOURCE_BLOB`, `CONTEXT_INIT`) si `num_capsets` se citesc si se raporteaza, fara sa fie activati.
- O coada, `controlq` (64 de intrari, 16 sloturi a cate 2 descriptori: cerere + raspuns). **Fara intreruperi**: `avail.flags = NO_INTERRUPT`, iar raspunsurile se culeg din inelul `used` la apelul urmator. Comenzile sincrone (create, attach, scanout, unref) asteapta cu spin, cu limita in timp si in iteratii (sub lock cu intreruperile oprite, jiffies nu avanseaza).
- La boot: `GET_DISPLAY_INFO` (modul preferat al gazdei) si `GET_CAPSET_INFO` pentru fiecare capset. Logul arata de exemplu `virtio-gpu: 2D 1024x768, scanout-uri 1, gazda 1280x800, 3D virgl: nu, blob: nu, context_init: nu, capset-uri 0`.
- Resursa 2D are id-ul 1, format `B8G8R8X8_UNORM` (adica xRGB 8:8:8 ca in ygfx) si geometria framebuffer-ului de boot, ca sa nu se schimbe layout-ul compozitorului. Fara fb, se ia modul gazdei. Se creeaza la **prima mapare** din user: pagini 4K alocate una cate una, apoi `RESOURCE_ATTACH_BACKING` cu alergarile de pagini consecutive si `SET_SCANOUT 0`. La **ultima demapare** urmeaza `SET_SCANOUT` cu resursa 0 (virtio-vga revine la VGA), `DETACH_BACKING`, `UNREF` si eliberarea paginilor.

### `/dev/gpu0`: `kernel/include/uapi/gpu.h`, copia user `<sys/ygpu.h>`

| Apel | Ce face |
|---|---|
| `ioctl(YGPU_IOC_INFO, &yk_gpu_info)` | geometrie, `stride`, `map_len`, `caps` (`YGPU_CAP_2D/VIRGL/EDID/UUID/BLOB/CTX_INIT/SCANOUT`), bitii bruti, scanout-uri, capset-uri (id, versiune, marime), modul gazdei; statistici: `cmds`, `flushes`, `bytes`, `errors`, `waits` |
| `mmap(MAP_SHARED, map_len)` | pixelii resursei: RAM obisnuit, cache WB (`PTE_DEV`, fara pageref, ca la fb0) |
| `ioctl(YGPU_IOC_FLUSH, &yk_gpu_flush)` | 1..8 dreptunghiuri: `TRANSFER_TO_HOST_2D` pe fiecare + un `RESOURCE_FLUSH` pe reuniune, intr-o singura notificare, **asincron**. `ENXIO` fara mapare, `EINVAL` daca n e invalid; dreptunghiurile se decupeaza la ecran |
| `ioctl(YGPU_IOC_SYNC)` | asteapta tot ce s-a trimis |
| `read`/`write` | `EINVAL` |

### Compozitorul: `user/bin/ycomp/backend_virtio.c`

- `ycomp -b auto|gpu|fb`. Implicit e `auto`: `/dev/gpu0` daca exista, altfel `/dev/fb0`. Mesajul de pornire arata `backend virtio-gpu` sau `backend fb0`.
- Pe gpu, `C.back` este chiar maparea resursei: compozitorul compune direct in ea, deci dispare back buffer-ul separat si copierea in memoria video write-combining. Flip-ul devine `vgpu_flush(dirty, n)`, adica un ioctl. Peste 8 zone murdare, restul se unesc in ultima.
- Fisierul e `.c`, nu `.cpp` cum cerea prompt-ul: userspace-ul nu are inca runtime C++, iar ycomp e scris integral in C.

### Masurare

Am adaugat `Task::cpu_ticks`, numarat in `sched_tick` pentru task-ul curent (fara idle). Il vezi ca `CpuMs:` in `/proc/<pid>/status`, cu rezolutie de 1 ms.

`make test-gpu` (test=gpu, inclus in `make test`). Ruleaza `/bin/gputest` cu QEMU `-vga std -device virtio-gpu-pci`. Partea 1 testeaza `/dev/gpu0` direct, cu 35 de verificari. Partea 2 porneste `ycomp -b gpu` si apoi `ycomp -b fb` cu acelasi client: o fereastra de 800x520 redesenata integral la fiecare cadru, 150 de cadre. Compara timpul CPU al ycomp pe cadru, iar pe gpu trebuie sa fie mai mic. In cloud (TCG, SMP 2/4, 3 rulari) a iesit:

| backend | CPU ycomp / cadru | timp in flip (total) |
|---|---|---|
| fb0 | ~14.6–16.0 ms | ~1.2–1.35 s |
| virtio-gpu | ~8.2–10.4 ms | ~10–14 ms |

Partea care ramane e compunerea in sine (amestec, blit-uri), care costa la fel pe ambele backend-uri. Sub KVM diferenta relativa e mai mica, pentru ca memoria VGA nu mai e emulata, dar copierea dispare oricum.

Pentru o rulare vizuala: `qemu-system-x86_64 -cdrom build/yokai.iso -m 512M -vga none -device virtio-vga`. GRUB si fbcon folosesc partea VGA, iar ycomp (`init -g`) trece singur pe scanout-ul virtio.

## Limitari cunoscute

- Un singur dispozitiv, un singur scanout (0) si o singura resursa. Nu exista double buffering: gazda poate citi o zona in timp ce compozitorul scrie urmatorul cadru, deci e posibil tearing.
- Fara intreruperi si fara fence-uri (`VIRTIO_GPU_FLAG_FENCE`). Fara cursor hardware (`cursorq`), cursorul ramane desenat software de ycomp. Nu se trateaza nici evenimentele de redimensionare a ferestrei gazdei (`VIRTIO_GPU_EVENT_DISPLAY`).
- 3D se detecteaza doar (bitul `VIRGL` si capset-urile). Nicio comanda 3D nu e implementata. In containerul cloud nu exista render node DRM, deci `virtio-gpu-gl-pci` nu a putut fi pornit: ramura „virgl: da / capset-uri > 0” a raportarii e scrisa dupa specificatie, dar netestata.

## Pasii spre Vulkan (Venus peste virtio-gpu)

Tinta: un ICD Vulkan in user (Mesa **Venus**) care trimite comenzi Vulkan serializate prin virtio-gpu catre virglrenderer pe gazda. Acolo ruleaza pe GPU-ul real. Ordinea propusa:

1. **Transport mai bun.** MSI-X pentru `controlq`/`cursorq` (`pci_msix_setup`) si fence-uri pe comenzi (`FLAG_FENCE` + `fence_id`), cu waitqueue in loc de spin. Fara ele, 3D nu poate astepta GPU-ul eficient.
2. **Afisare completa 2D.** Doua resurse cu page flip la `SET_SCANOUT` pe fence (fara tearing), `cursorq` (`UPDATE_CURSOR`/`MOVE_CURSOR`), `EDID` si `EVENT_DISPLAY` (rezolutie dinamica), mai multe scanout-uri.
3. **ABI de tip DRM.** In loc sa crestem `YGPU_IOC_*`, portam setul de ioctl-uri `virtgpu_drm.h` din Linux: `GETPARAM`, `GET_CAPS`, `RESOURCE_CREATE`, `RESOURCE_CREATE_BLOB`, `MAP`, `EXECBUFFER`, `WAIT`, `TRANSFER_TO/FROM_HOST`, `CONTEXT_INIT`. Pe acest ABI ruleaza Mesa nemodificat. Mai trebuie handle-uri GEM per fd si `close` pe resursa.
4. **3D de baza (virgl).** Negocierea `VIRGL`, `CTX_CREATE`/`CTX_DESTROY`, `CTX_ATTACH/DETACH_RESOURCE`, `RESOURCE_CREATE_3D`, `TRANSFER_*_3D`, `SUBMIT_3D`, `GET_CAPSET`. Cu ele se poate testa un client minim care trimite un buffer virgl (clear + present), fara Mesa.
5. **Blob si memoria gazdei.** Capabilitatea `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` (id 8, regiunea „hostmem”), `RESOURCE_CREATE_BLOB`, `RESOURCE_MAP_BLOB`/`UNMAP_BLOB` si mmap in user cu caching-ul cerut de gazda (`map_info`). Venus are nevoie de ele pentru memoria vizibila de pe host.
6. **`CONTEXT_INIT` cu capset Venus (4).** Contexte create cu `context_init = VENUS`, inel de comenzi Venus in memorie partajata si sincronizare prin fence-uri pe context si inel.
7. **Userspace.** Libc trebuie sa ajunga la ce cere Mesa: pthread si futex, `dlopen` sau un ICD legat static, `clock_gettime`, `poll` pe fd-ul GPU, `memfd`/`shm`. Dupa asta se porteaza Mesa (doar `src/virtio/vulkan` + `util`, build cu meson pentru tinta YokaiOS) si un loader Vulkan minim, sau ICD-ul apelat direct.
8. **Compozitorul pe Vulkan.** Un backend `backend_vulkan` in ycomp: ferestrele devin texturi (shm importat ca blob sau copiat), compunerea pe GPU, apoi `SET_SCANOUT_BLOB` sau prezentare prin resursa 2D.
9. **Testare.** QEMU >= 9.2 cu virglrenderer compilat cu Venus: `-device virtio-gpu-gl-pci,blob=true,hostmem=4G,venus=true -display sdl,gl=on` (sau `egl-headless` pe o gazda cu render node). In CI cloud nu exista GPU, deci testul 3D trebuie sa se poata sari, ca `test-rt` strict fara KVM.

GPU-urile reale (Intel i915/xe, AMD amdgpu) raman in afara planului: fiecare ar insemna un driver DRM complet.
