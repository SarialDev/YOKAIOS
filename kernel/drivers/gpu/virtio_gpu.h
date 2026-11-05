#pragma once
#include "types.h"

// virtio-gpu (T10.6): transport virtio 1.x prin PCI (capabilitati vendor), coada de control, comenzi 2D.
// Nu foloseste intreruperi: comenzile se trimit cu o notificare, raspunsurile se culeg din inelul "used" la apelul urmator
// (sau se asteapta cu spin, cu limita de timp, pentru comenzile sincrone). 3D (virgl) / blob / context_init doar se detecteaza
// si se raporteaza (docs/GPU.md: pasii spre Vulkan prin Venus).
//
// Userul il vede ca /dev/gpu0 (fs/devfs.cpp): ioctl YGPU_IOC_* (uapi/gpu.h) si mmap pe pixelii resursei 2D.

struct yk_gpu_info;
struct yk_gpu_flush;

// Inregistreaza driverul PCI (1af4:1050, virtio-gpu-pci si virtio-vga)
void virtio_gpu_init();
bool virtio_gpu_ready();

// Informatii + statistici (fara efecte); false fara dispozitiv
bool virtio_gpu_info(yk_gpu_info& out);

// Maparile userului: prima creeaza resursa 2D, ii ataseaza paginile si o pune pe scanout 0; ultima o scoate si elibereaza paginile
void virtio_gpu_user_get();
void virtio_gpu_user_put();
bool virtio_gpu_user_owned();
// Lungimea mapabila (pagini intregi) si adresa fizica a paginii de la off; -ENXIO fara resursa / in afara ei
uint64_t virtio_gpu_map_len();
int64_t virtio_gpu_map_phys(uint64_t off);

// TRANSFER_TO_HOST_2D pe fiecare dreptunghi + RESOURCE_FLUSH pe reuniune (asincron). 0 sau -errno
int virtio_gpu_flush(const yk_gpu_flush& f);
// Asteapta toate comenzile trimise; 0 sau -ETIMEDOUT
int virtio_gpu_sync();

// Comenzi trimise de la boot (testul kernelului verifica ca s-a lucrat pe dispozitiv)
uint64_t virtio_gpu_cmd_count();
