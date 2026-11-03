/* ygfx: ecranul prin /dev/fb0 (T10.1): ioctl FBIOGET_* pentru geometrie si format, mmap MAP_SHARED pe memoria video.
 * Desenul se face de obicei intr-o suprafata din RAM si se copiaza cu yg_fb_present: memoria video e write-combining,
 * deci citirile (amestecul alfa citeste destinatia) ar fi foarte lente direct pe ea. */
#include "ygfx.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int yg_fb_open(YgFb *fb, const char *path){
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
    int fd = open(path ? path : "/dev/fb0", O_RDWR);
    if(fd < 0){
        return -errno;
    }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if(ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0){
        int e = errno;
        close(fd);
        return -e;
    }
    if(var.bits_per_pixel != 32 || var.xres == 0 || var.yres == 0 || fix.line_length < var.xres * 4){
        close(fd);
        return -EOPNOTSUPP;
    }
    void *m = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(m == MAP_FAILED){
        int e = errno;
        close(fd);
        return -e;
    }
    fb->fd = fd;
    fb->map = m;
    fb->map_len = fix.smem_len;
    fb->width = (int)var.xres;
    fb->height = (int)var.yres;
    fb->pitch = (int)fix.line_length;
    fb->shift[0] = (uint8_t)var.red.offset;
    fb->shift[1] = (uint8_t)var.green.offset;
    fb->shift[2] = (uint8_t)var.blue.offset;
    fb->bits[0] = (uint8_t)var.red.length;
    fb->bits[1] = (uint8_t)var.green.length;
    fb->bits[2] = (uint8_t)var.blue.length;
    fb->native = fb->shift[0] == 16 && fb->shift[1] == 8 && fb->shift[2] == 0 &&
                 fb->bits[0] == 8 && fb->bits[1] == 8 && fb->bits[2] == 8;
    return 0;
}

void yg_fb_close(YgFb *fb){
    if(fb->map){
        munmap(fb->map, fb->map_len);
        fb->map = NULL;
    }
    if(fb->fd >= 0){
        close(fb->fd);
        fb->fd = -1;
    }
}

static inline uint32_t convert(const YgFb *fb, YgColor c){
    const uint8_t v[3] = {YG_R(c), YG_G(c), YG_B(c)};
    uint32_t out = 0;
    for(int i = 0; i < 3; i++){
        out |= (uint32_t)(v[i] >> (8 - fb->bits[i])) << fb->shift[i];
    }
    return out;
}

void yg_fb_present(YgFb *fb, const YgSurface *src, const YgRect *area){
    if(!fb->map){
        return;
    }
    YgRect r = yg_rect(0, 0, src->width, src->height);
    if(area){
        r = yg_rect_intersect(r, *area);
    }
    r = yg_rect_intersect(r, yg_rect(0, 0, fb->width, fb->height));
    for(int y = r.y; y < r.y + r.h; y++){
        const uint32_t *s = &src->pixels[(size_t)y * src->stride + r.x];
        uint32_t *d = (uint32_t *)(fb->map + (size_t)y * fb->pitch) + r.x;
        if(fb->native){
            memcpy(d, s, (size_t)r.w * 4);
        } else {
            for(int i = 0; i < r.w; i++){
                d[i] = convert(fb, s[i]);
            }
        }
    }
}

int yg_fb_surface(YgFb *fb, YgSurface *out){
    if(!fb->map || !fb->native || fb->pitch % 4){
        return -1;
    }
    yg_surface_wrap(out, (uint32_t *)fb->map, fb->width, fb->height, fb->pitch / 4);
    return 0;
}
