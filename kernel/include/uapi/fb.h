#pragma once
/* /dev/fb0 (T10.1): ioctl-urile FBIOGET_* cu acelasi numar si layout ca pe Linux x86-64 (linux/fb.h), ca bibliotecile
 * portate sa poata citi geometria. Kernelul o foloseste in fs/devfs.cpp; libc are copia in <linux/fb.h>. */
#include "types.h"

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

#define FB_TYPE_PACKED_PIXELS   0
#define FB_VISUAL_TRUECOLOR     2

struct KFbBitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct KFbVarInfo {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    KFbBitfield red, green, blue, transp;
    uint32_t nonstd, activate;
    uint32_t height, width;             /* mm, 0 = necunoscut */
    uint32_t accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len;
    uint32_t sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct KFbFixInfo {
    char id[16];
    uint64_t smem_start;                /* adresa fizica; YokaiOS intoarce 0 (nu se divulga) */
    uint32_t smem_len;                  /* bytes mapabili cu mmap (pitch * yres, rotunjit la pagina) */
    uint32_t type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;               /* pitch in bytes */
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

static_assert(sizeof(KFbVarInfo) == 160, "fb_var_screeninfo");
static_assert(sizeof(KFbFixInfo) == 80, "fb_fix_screeninfo");
