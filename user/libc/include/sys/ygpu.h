#ifndef _SYS_YGPU_H
#define _SYS_YGPU_H
#include <stdint.h>
/* /dev/gpu0 (T10.6): virtio-gpu 2D ca backend de afisare. Copie a kernel/include/uapi/gpu.h;
 * aceleasi structuri pentru programe (ycomp backend_virtio.c, gputest).
 *
 *   open("/dev/gpu0") + ioctl(YGPU_IOC_INFO): geometria, capabilitatile gazdei (3D/virgl, blob, context_init, capset-uri)
 *   mmap(MAP_SHARED, info.map_len): pixelii resursei 2D (xRGB 8:8:8, stride = info.stride), RAM obisnuit (cache WB):
 *       prima mapare creeaza resursa, ii ataseaza paginile si o pune pe scanout 0; ultima demapare o scoate
 *   ioctl(YGPU_IOC_FLUSH, &f): TRANSFER_TO_HOST_2D pentru fiecare dreptunghi + RESOURCE_FLUSH pe reuniunea lor.
 *       Asincron: comenzile pleaca intr-o singura notificare, raspunsurile se culeg la urmatorul apel */

#define YGPU_ABI            1
#define YGPU_IOC_INFO       0x5900
#define YGPU_IOC_FLUSH      0x5901
#define YGPU_IOC_SYNC       0x5902      /* asteapta toate comenzile trimise (pentru teste / capturi) */

#define YGPU_FMT_XRGB8888   1           /* = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM: octetii B, G, R, X */

/* caps */
#define YGPU_CAP_2D         (1u << 0)   /* mereu */
#define YGPU_CAP_VIRGL      (1u << 1)   /* VIRTIO_GPU_F_VIRGL: 3D prin virglrenderer (OpenGL) */
#define YGPU_CAP_EDID       (1u << 2)
#define YGPU_CAP_UUID       (1u << 3)   /* VIRTIO_GPU_F_RESOURCE_UUID */
#define YGPU_CAP_BLOB       (1u << 4)   /* VIRTIO_GPU_F_RESOURCE_BLOB: memorie partajata gazda (necesar pt. Venus/Vulkan) */
#define YGPU_CAP_CTX_INIT   (1u << 5)   /* VIRTIO_GPU_F_CONTEXT_INIT: contexte cu capset ales (Venus = capset 4) */
#define YGPU_CAP_SCANOUT    (1u << 8)   /* resursa e acum pe ecran (exista o mapare) */

/* capset-uri cunoscute (virtio-gpu) */
#define YGPU_CAPSET_VIRGL   1
#define YGPU_CAPSET_VIRGL2  2
#define YGPU_CAPSET_GFXSTREAM 3
#define YGPU_CAPSET_VENUS   4
#define YGPU_CAPSET_DRM     6

#define YGPU_MAX_CAPSETS    8
#define YGPU_MAX_RECTS      8

struct yk_gpu_capset {
    uint32_t id;
    uint32_t max_version;
    uint32_t max_size;
    uint32_t pad;
};

struct yk_gpu_info {
    uint32_t abi;               /* YGPU_ABI */
    uint32_t width, height;     /* resursa / modul afisat */
    uint32_t stride;            /* octeti pe linie in maparea mmap */
    uint32_t format;            /* YGPU_FMT_* */
    uint32_t map_len;           /* octeti de mapat (multiplu de pagina) */
    uint32_t caps;              /* YGPU_CAP_* */
    uint32_t features;          /* bitii 0..31 ai dispozitivului, bruti */
    uint32_t num_scanouts;
    uint32_t num_capsets;       /* din configul dispozitivului */
    uint32_t host_w, host_h;    /* modul preferat al scanout-ului 0 raportat de gazda (GET_DISPLAY_INFO) */
    struct yk_gpu_capset capset[YGPU_MAX_CAPSETS];
    /* statistici de la boot */
    uint64_t cmds;              /* comenzi trimise */
    uint64_t flushes;           /* apeluri YGPU_IOC_FLUSH */
    uint64_t bytes;             /* pixeli transferati * 4 */
    uint64_t errors;            /* raspunsuri de eroare de la gazda */
    uint64_t waits;             /* de cate ori a trebuit asteptat un slot liber */
};

struct yk_gpu_rect {
    uint32_t x, y, w, h;
};

struct yk_gpu_flush {
    uint32_t n;                 /* 1..YGPU_MAX_RECTS */
    uint32_t pad;
    struct yk_gpu_rect r[YGPU_MAX_RECTS];
};
#endif
