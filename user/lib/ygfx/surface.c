/* ygfx: suprafete, clipping, amestec alfa, primitive dreptunghiulare, linii Bresenham, blit */
#include "ygfx.h"
#include "ygfx_priv.h"
#include <stdlib.h>
#include <string.h>

YgRect yg_rect(int x, int y, int w, int h){
    YgRect r = {x, y, w, h};
    return r;
}

YgRect yg_rect_intersect(YgRect a, YgRect b){
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if(x1 <= x0 || y1 <= y0){
        return yg_rect(x0, y0, 0, 0);
    }
    return yg_rect(x0, y0, x1 - x0, y1 - y0);
}

int yg_rect_empty(YgRect r){
    return r.w <= 0 || r.h <= 0;
}

int yg_rect_contains(YgRect r, int x, int y){
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

YgSurface *yg_surface_new(int w, int h){
    if(w <= 0 || h <= 0 || w > 16384 || h > 16384){
        return NULL;
    }
    YgSurface *s = malloc(sizeof(*s));
    if(!s){
        return NULL;
    }
    s->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if(!s->pixels){
        free(s);
        return NULL;
    }
    s->width = w;
    s->height = h;
    s->stride = w;
    s->owned = 1;
    s->clip = yg_rect(0, 0, w, h);
    return s;
}

void yg_surface_wrap(YgSurface *s, uint32_t *pixels, int w, int h, int stride){
    s->pixels = pixels;
    s->width = w;
    s->height = h;
    s->stride = stride;
    s->owned = 0;
    s->clip = yg_rect(0, 0, w, h);
}

void yg_surface_free(YgSurface *s){
    if(!s){
        return;
    }
    if(s->owned){
        free(s->pixels);
        free(s);
    }
}

YgRect yg_set_clip(YgSurface *s, YgRect r){
    YgRect old = s->clip;
    s->clip = yg_rect_intersect(r, yg_rect(0, 0, s->width, s->height));
    return old;
}

void yg_reset_clip(YgSurface *s){
    s->clip = yg_rect(0, 0, s->width, s->height);
}

/* ---- culori ---- */

/* x / 255 rotunjit, pentru x in [0, 255 * 255] */
static inline uint32_t div255(uint32_t x){
    return (x + 1 + (x >> 8)) >> 8;
}

YgColor yg_blend(YgColor dst, YgColor src){
    uint32_t sa = YG_A(src);
    if(sa == 255){
        return src;
    }
    if(sa == 0){
        return dst;
    }
    uint32_t da = YG_A(dst);
    if(da == 255){
        /* destinatie opaca (cazul obisnuit): c = d + (s - d) * sa */
        uint32_t ia = 255 - sa;
        uint32_t r = div255(YG_R(src) * sa + YG_R(dst) * ia);
        uint32_t g = div255(YG_G(src) * sa + YG_G(dst) * ia);
        uint32_t b = div255(YG_B(src) * sa + YG_B(dst) * ia);
        return YG_ARGB(255, r, g, b);
    }
    /* general: oa = sa + da (1 - sa), oc = (sc sa + dc da (1 - sa)) / oa */
    uint32_t dw = div255(da * (255 - sa));
    uint32_t oa = sa + dw;
    if(oa == 0){
        return 0;
    }
    uint32_t r = (YG_R(src) * sa + YG_R(dst) * dw + oa / 2) / oa;
    uint32_t g = (YG_G(src) * sa + YG_G(dst) * dw + oa / 2) / oa;
    uint32_t b = (YG_B(src) * sa + YG_B(dst) * dw + oa / 2) / oa;
    return YG_ARGB(oa, r, g, b);
}

YgColor yg_mix(YgColor a, YgColor b, uint8_t t){
    uint32_t it = 255 - t;
    return YG_ARGB(div255(YG_A(a) * it + YG_A(b) * t), div255(YG_R(a) * it + YG_R(b) * t),
                   div255(YG_G(a) * it + YG_G(b) * t), div255(YG_B(a) * it + YG_B(b) * t));
}

YgColor yg_get_pixel(const YgSurface *s, int x, int y){
    if(x < 0 || y < 0 || x >= s->width || y >= s->height){
        return 0;
    }
    return s->pixels[(size_t)y * s->stride + x];
}

void yg_set_pixel(YgSurface *s, int x, int y, YgColor c){
    if(yg_rect_contains(s->clip, x, y)){
        s->pixels[(size_t)y * s->stride + x] = c;
    }
}

void yg_blend_pixel(YgSurface *s, int x, int y, YgColor c){
    if(yg_rect_contains(s->clip, x, y)){
        uint32_t *p = &s->pixels[(size_t)y * s->stride + x];
        *p = yg_blend(*p, c);
    }
}

/* Amesteca c cu acoperirea cov (0..255) peste pixelul p */
void yg__blend_cov(uint32_t *p, YgColor c, uint32_t cov){
    uint32_t a = div255(YG_A(c) * cov);
    if(a){
        *p = yg_blend(*p, YG_WITH_ALPHA(c, a));
    }
}

/* Un rand orizontal [x0, x1) in clip deja verificat */
static void span(YgSurface *s, int y, int x0, int x1, YgColor c){
    uint32_t *p = &s->pixels[(size_t)y * s->stride + x0];
    int n = x1 - x0;
    if(YG_A(c) == 255){
        for(int i = 0; i < n; i++){
            p[i] = c;
        }
    } else if(YG_A(c)){
        for(int i = 0; i < n; i++){
            p[i] = yg_blend(p[i], c);
        }
    }
}

/* ---- primitive ---- */

void yg_clear(YgSurface *s, YgColor c){
    YgRect r = s->clip;
    for(int y = r.y; y < r.y + r.h; y++){
        uint32_t *p = &s->pixels[(size_t)y * s->stride + r.x];
        for(int i = 0; i < r.w; i++){
            p[i] = c;
        }
    }
}

void yg_fill_rect(YgSurface *s, int x, int y, int w, int h, YgColor c){
    YgRect r = yg_rect_intersect(yg_rect(x, y, w, h), s->clip);
    for(int row = r.y; row < r.y + r.h; row++){
        span(s, row, r.x, r.x + r.w, c);
    }
}

void yg_draw_rect(YgSurface *s, int x, int y, int w, int h, int t, YgColor c){
    if(w <= 0 || h <= 0 || t <= 0){
        return;
    }
    if(t * 2 >= w || t * 2 >= h){
        yg_fill_rect(s, x, y, w, h, c);
        return;
    }
    yg_fill_rect(s, x, y, w, t, c);                 /* sus */
    yg_fill_rect(s, x, y + h - t, w, t, c);         /* jos */
    yg_fill_rect(s, x, y + t, t, h - 2 * t, c);     /* stanga */
    yg_fill_rect(s, x + w - t, y + t, t, h - 2 * t, c);
}

void yg_draw_line(YgSurface *s, int x0, int y0, int x1, int y1, YgColor c){
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;    /* negativ */
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    /* Linia intreaga in afara clip-ului (pe o axa): nimic de facut */
    YgRect r = s->clip;
    if((x0 < r.x && x1 < r.x) || (y0 < r.y && y1 < r.y) || (x0 >= r.x + r.w && x1 >= r.x + r.w) ||
       (y0 >= r.y + r.h && y1 >= r.y + r.h)){
        return;
    }
    for(;;){
        yg_blend_pixel(s, x0, y0, c);
        if(x0 == x1 && y0 == y1){
            break;
        }
        int e2 = 2 * err;
        if(e2 >= dy){
            err += dy;
            x0 += sx;
        }
        if(e2 <= dx){
            err += dx;
            y0 += sy;
        }
    }
}

void yg_fill_gradient_v(YgSurface *s, int x, int y, int w, int h, YgColor top, YgColor bottom){
    if(h <= 0){
        return;
    }
    YgRect r = yg_rect_intersect(yg_rect(x, y, w, h), s->clip);
    for(int row = r.y; row < r.y + r.h; row++){
        int t = h > 1 ? (row - y) * 255 / (h - 1) : 0;
        span(s, row, r.x, r.x + r.w, yg_mix(top, bottom, (uint8_t)t));
    }
}

void yg_fill_mask(YgSurface *s, int x, int y, const uint8_t *mask, int w, int h, int mask_stride, YgColor c){
    YgRect r = yg_rect_intersect(yg_rect(x, y, w, h), s->clip);
    for(int row = r.y; row < r.y + r.h; row++){
        const uint8_t *m = mask + (size_t)(row - y) * mask_stride + (r.x - x);
        uint32_t *p = &s->pixels[(size_t)row * s->stride + r.x];
        for(int i = 0; i < r.w; i++){
            if(m[i]){
                yg__blend_cov(&p[i], c, m[i]);
            }
        }
    }
}

/* ---- blit ---- */

/* Dreptunghiul sursa si destinatia, decupate; 0 daca nu ramane nimic */
static int blit_rects(const YgSurface *dst, int *dx, int *dy, const YgSurface *src, const YgRect *src_rect, YgRect *sr){
    YgRect r = src_rect ? yg_rect_intersect(*src_rect, yg_rect(0, 0, src->width, src->height))
                        : yg_rect(0, 0, src->width, src->height);
    /* deplasarea fata de src_rect cerut (daca a fost taiat la marginile sursei) */
    if(src_rect){
        *dx += r.x - src_rect->x;
        *dy += r.y - src_rect->y;
    }
    YgRect d = yg_rect_intersect(yg_rect(*dx, *dy, r.w, r.h), dst->clip);
    if(yg_rect_empty(d)){
        return 0;
    }
    sr->x = r.x + (d.x - *dx);
    sr->y = r.y + (d.y - *dy);
    sr->w = d.w;
    sr->h = d.h;
    *dx = d.x;
    *dy = d.y;
    return 1;
}

void yg_blit(YgSurface *dst, int dx, int dy, const YgSurface *src, const YgRect *src_rect){
    YgRect sr;
    if(!blit_rects(dst, &dx, &dy, src, src_rect, &sr)){
        return;
    }
    for(int row = 0; row < sr.h; row++){
        memmove(&dst->pixels[(size_t)(dy + row) * dst->stride + dx],
                &src->pixels[(size_t)(sr.y + row) * src->stride + sr.x], (size_t)sr.w * 4);
    }
}

void yg_blit_alpha(YgSurface *dst, int dx, int dy, const YgSurface *src, const YgRect *src_rect, uint8_t opacity){
    YgRect sr;
    if(!opacity || !blit_rects(dst, &dx, &dy, src, src_rect, &sr)){
        return;
    }
    for(int row = 0; row < sr.h; row++){
        uint32_t *d = &dst->pixels[(size_t)(dy + row) * dst->stride + dx];
        const uint32_t *sp = &src->pixels[(size_t)(sr.y + row) * src->stride + sr.x];
        for(int i = 0; i < sr.w; i++){
            YgColor c = sp[i];
            if(opacity != 255){
                c = YG_WITH_ALPHA(c, div255(YG_A(c) * opacity));
            }
            d[i] = yg_blend(d[i], c);
        }
    }
}
