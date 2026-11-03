#ifndef _LINUX_FB_H
#define _LINUX_FB_H
#include <stdint.h>
/* /dev/fb0 (T10.1): ioctl FBIOGET_* ca pe Linux x86-64 (copie a kernel/include/uapi/fb.h).
 * Ecranul se mapeaza cu mmap(MAP_SHARED) pe fd-ul lui /dev/fb0: smem_len bytes, line_length bytes pe linie.
 * Cat timp exista o mapare, consola nu mai deseneaza pe ecran; la munmap / exit ecranul consolei revine. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR 2

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate;
    uint32_t height, width;
    uint32_t accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len;
    uint32_t sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len;
    uint32_t type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};
#endif
