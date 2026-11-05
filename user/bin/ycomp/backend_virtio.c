/* ycomp (T10.6): backend virtio-gpu 2D prin /dev/gpu0.
 * Compozitorul deseneaza direct in pixelii resursei 2D (RAM obisnuit mapat cu mmap, cache write-back), deci nu mai exista
 * back buffer separat si nici copierea zonelor murdare in memoria video write-combining a lui /dev/fb0. "Flip"-ul e un
 * singur ioctl YGPU_IOC_FLUSH: kernelul trimite TRANSFER_TO_HOST_2D pe fiecare dreptunghi + RESOURCE_FLUSH, intr-o
 * singura notificare, fara sa astepte gazda (QEMU copiaza pixelii pe firul lui).
 * Fisierul e C (ca restul ycomp; userspace-ul YokaiOS nu are inca runtime C++). */
#include "comp.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ygpu.h>

static int gpu_fd = -1;
static void *gpu_map;
static size_t gpu_len;
static YgSurface gpu_surf;
static struct yk_gpu_info gpu_info;

int vgpu_open(void){
    int fd = open("/dev/gpu0", O_RDWR);
    if(fd < 0){
        return -errno;
    }
    if(ioctl(fd, YGPU_IOC_INFO, &gpu_info) < 0){
        int e = errno;
        close(fd);
        return -e;
    }
    if(gpu_info.abi != YGPU_ABI || gpu_info.format != YGPU_FMT_XRGB8888 || gpu_info.stride % 4 ||
       gpu_info.width == 0 || gpu_info.height == 0){
        close(fd);
        return -EOPNOTSUPP;
    }
    void *m = mmap(NULL, gpu_info.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(m == MAP_FAILED){
        int e = errno;
        close(fd);
        return -e;
    }
    gpu_fd = fd;
    gpu_map = m;
    gpu_len = gpu_info.map_len;
    yg_surface_wrap(&gpu_surf, (uint32_t *)m, (int)gpu_info.width, (int)gpu_info.height, (int)(gpu_info.stride / 4));
    C.sw = (int)gpu_info.width;
    C.sh = (int)gpu_info.height;
    C.back = &gpu_surf;
    return 0;
}

/* Trimite zonele murdare (cel mult YGPU_MAX_RECTS; restul se unesc in ultimul) */
void vgpu_flush(const YgRect *r, int n){
    if(gpu_fd < 0 || n <= 0){
        return;
    }
    struct yk_gpu_flush f;
    memset(&f, 0, sizeof(f));
    int k = 0;
    for(int i = 0; i < n; i++){
        if(k < YGPU_MAX_RECTS){
            f.r[k].x = (uint32_t)r[i].x;
            f.r[k].y = (uint32_t)r[i].y;
            f.r[k].w = (uint32_t)r[i].w;
            f.r[k].h = (uint32_t)r[i].h;
            k++;
        } else {
            /* reuniunea cu ultimul dreptunghi trimis */
            struct yk_gpu_rect *u = &f.r[YGPU_MAX_RECTS - 1];
            uint32_t x0 = u->x < (uint32_t)r[i].x ? u->x : (uint32_t)r[i].x;
            uint32_t y0 = u->y < (uint32_t)r[i].y ? u->y : (uint32_t)r[i].y;
            uint32_t x1 = u->x + u->w > (uint32_t)(r[i].x + r[i].w) ? u->x + u->w : (uint32_t)(r[i].x + r[i].w);
            uint32_t y1 = u->y + u->h > (uint32_t)(r[i].y + r[i].h) ? u->y + u->h : (uint32_t)(r[i].y + r[i].h);
            u->x = x0;
            u->y = y0;
            u->w = x1 - x0;
            u->h = y1 - y0;
        }
    }
    f.n = (uint32_t)k;
    ioctl(gpu_fd, YGPU_IOC_FLUSH, &f);
}

void vgpu_close(void){
    if(gpu_map){
        munmap(gpu_map, gpu_len);
        gpu_map = NULL;
    }
    if(gpu_fd >= 0){
        close(gpu_fd);
        gpu_fd = -1;
    }
}

const struct yk_gpu_info *vgpu_info(void){
    return gpu_fd >= 0 ? &gpu_info : NULL;
}
