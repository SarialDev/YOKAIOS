/* ygfx: rasterizare cu antialias. Fiecare muchie a conturului adauga in acumulatorul unui rand aria (cu semn) pe care
 * o acopera in fiecare celula; suma pe rand de la stanga la dreapta da acoperirea exacta a pixelului (aceeasi idee ca
 * rasterizatorul din font-rs / stb_truetype v2). |suma| > 1 (contururi suprapuse in acelasi sens) se taie la 1 =>
 * regula nonzero pentru forme simple, iar un contur in sens invers scade aria (gauri). */
#include "ygfx.h"
#include "ygfx_priv.h"
#include <stdlib.h>
#include <math.h>

/* Muchia (x0, y0) -> (x1, y1) in coordonatele bufferului: x in [0, W], randuri [0, H), st = W + 2 */
static void acc_line(float *acc, int st, int H, float x0, float y0, float x1, float y1){
    if(y0 == y1){
        return;
    }
    float dir = 1.0f;
    if(y0 > y1){
        float t;
        dir = -1.0f;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    int ys;
    if(y0 < 0.0f){
        x -= y0 * dxdy;
        ys = 0;
    } else {
        ys = (int)y0;
    }
    int ye = (int)yg__ceil(y1);
    if(ye > H){
        ye = H;
    }
    for(int y = ys; y < ye; y++){
        float *row = acc + (size_t)y * st;
        float ytop = (float)y > y0 ? (float)y : y0;
        float ybot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = ybot - ytop;
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xnext ? x : xnext;
        float xb = x < xnext ? xnext : x;
        /* rotunjirile pot iesi putin din [0, W] */
        if(xa < 0.0f) xa = 0.0f;
        if(xb > (float)(st - 2)) xb = (float)(st - 2);
        if(xb < xa) xb = xa;
        float xa_floor = yg__floor(xa);
        int xai = (int)xa_floor;
        float xb_ceil = yg__ceil(xb);
        int xbi = (int)xb_ceil;
        if(xbi <= xai + 1){
            float xmf = 0.5f * (xa + xb) - xa_floor;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float xaf = xa - xa_floor;
            float a0 = 0.5f * s * (1.0f - xaf) * (1.0f - xaf);
            float xbf = xb - xb_ceil + 1.0f;
            float am = 0.5f * s * xbf * xbf;
            row[xai] += d * a0;
            if(xbi == xai + 2){
                row[xai + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - xaf);
                row[xai + 1] += d * (a1 - a0);
                for(int xi = xai + 2; xi < xbi - 1; xi++){
                    row[xi] += d * s;
                }
                float a2 = a1 + (float)(xbi - xai - 3) * s;
                row[xbi - 1] += d * (1.0f - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

void yg_fill_path(YgSurface *s, const YgPoint *pts, const int *counts, int ncontours, YgColor c){
    if(!YG_A(c) || ncontours <= 0){
        return;
    }
    int total = 0;
    for(int i = 0; i < ncontours; i++){
        total += counts[i];
    }
    if(total < 3){
        return;
    }
    float minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for(int i = 1; i < total; i++){
        if(pts[i].x < minx) minx = pts[i].x;
        if(pts[i].x > maxx) maxx = pts[i].x;
        if(pts[i].y < miny) miny = pts[i].y;
        if(pts[i].y > maxy) maxy = pts[i].y;
    }
    if(minx < -1e6f || maxx > 1e6f || miny < -1e6f || maxy > 1e6f){
        return;
    }
    int bx0 = (int)yg__floor(minx), by0 = (int)yg__floor(miny);
    int bx1 = (int)yg__ceil(maxx), by1 = (int)yg__ceil(maxy);
    YgRect b = yg_rect_intersect(yg_rect(bx0, by0, bx1 - bx0, by1 - by0), s->clip);
    if(yg_rect_empty(b)){
        return;
    }
    int W = b.w, H = b.h, st = W + 2;
    float *acc = calloc((size_t)st * (size_t)H, sizeof(float));
    if(!acc){
        return;
    }
    const YgPoint *p = pts;
    for(int k = 0; k < ncontours; k++){
        int n = counts[k];
        for(int i = 0; i < n; i++){
            YgPoint a = p[i], e = p[(i + 1) % n];
            float ax = a.x - (float)b.x, ex = e.x - (float)b.x;
            /* la stanga bufferului muchia se proiecteaza pe x = 0 (contribuie la tot randul), la dreapta nu conteaza */
            if(ax < 0.0f) ax = 0.0f;
            if(ex < 0.0f) ex = 0.0f;
            if(ax > (float)W) ax = (float)W;
            if(ex > (float)W) ex = (float)W;
            acc_line(acc, st, H, ax, a.y - (float)b.y, ex, e.y - (float)b.y);
        }
        p += n;
    }
    for(int y = 0; y < H; y++){
        const float *row = acc + (size_t)y * st;
        uint32_t *px = &s->pixels[(size_t)(b.y + y) * s->stride + b.x];
        float sum = 0.0f;
        for(int x = 0; x < W; x++){
            sum += row[x];
            float cov = sum < 0.0f ? -sum : sum;
            if(cov > 0.002f){
                uint32_t cv = cov >= 0.998f ? 255u : (uint32_t)(cov * 255.0f + 0.5f);
                if(cv == 255u && YG_A(c) == 255){
                    px[x] = c;
                } else {
                    yg__blend_cov(&px[x], c, cv);
                }
            }
        }
    }
    free(acc);
}

void yg_fill_polygon(YgSurface *s, const YgPoint *pts, int n, YgColor c){
    yg_fill_path(s, pts, &n, 1, c);
}

/* ---- contururi ---- */

static int circle_segments(float r){
    int n = (int)(r * 0.75f) + 12;
    return n > 360 ? 360 : n;
}

/* Arc de cerc in out (n + 1 puncte, de la a0 la a1 inclusiv), unghiuri in radiani, y in jos */
static int arc(YgPoint *out, float cx, float cy, float r, double a0, double a1, int n){
    double step = (a1 - a0) / n;
    double cs = cos(step), sn = sin(step);
    double dx = cos(a0) * r, dy = sin(a0) * r;
    for(int i = 0; i <= n; i++){
        out[i].x = cx + (float)dx;
        out[i].y = cy + (float)dy;
        double nx = dx * cs - dy * sn;
        dy = dx * sn + dy * cs;
        dx = nx;
    }
    return n + 1;
}

/* Cerc complet ca poligon; reverse = sens invers (gaura) */
static int circle_contour(YgPoint *out, float cx, float cy, float r, int n, int reverse){
    double a0 = 0.0, a1 = 2.0 * YG_PI * (n - 1) / n;
    if(reverse){
        a0 = 2.0 * YG_PI;
        a1 = 2.0 * YG_PI / n;
    }
    return arc(out, cx, cy, r, a0, a1, n - 1);
}

void yg_fill_circle(YgSurface *s, float cx, float cy, float r, YgColor c){
    if(r <= 0.0f){
        return;
    }
    int n = circle_segments(r);
    YgPoint *pts = malloc(sizeof(YgPoint) * (size_t)n);
    if(!pts){
        return;
    }
    circle_contour(pts, cx, cy, r, n, 0);
    yg_fill_polygon(s, pts, n, c);
    free(pts);
}

void yg_draw_circle(YgSurface *s, float cx, float cy, float r, float t, YgColor c){
    if(t <= 0.0f || r <= 0.0f){
        return;
    }
    float ro = r + t * 0.5f, ri = r - t * 0.5f;
    if(ri <= 0.0f){
        yg_fill_circle(s, cx, cy, ro, c);
        return;
    }
    int n = circle_segments(ro);
    YgPoint *pts = malloc(sizeof(YgPoint) * (size_t)n * 2);
    if(!pts){
        return;
    }
    int counts[2];
    counts[0] = circle_contour(pts, cx, cy, ro, n, 0);
    counts[1] = circle_contour(pts + n, cx, cy, ri, n, 1);
    yg_fill_path(s, pts, counts, 2, c);
    free(pts);
}

/* Contur de dreptunghi rotunjit (in sensul acelor de ceasornic pe ecran, sau invers); intoarce numarul de puncte */
static int rrect_contour(YgPoint *out, float x, float y, float w, float h, float r, int seg, int reverse){
    int n = 0;
    if(r <= 0.0f){
        out[0].x = x;     out[0].y = y;
        out[1].x = x + w; out[1].y = y;
        out[2].x = x + w; out[2].y = y + h;
        out[3].x = x;     out[3].y = y + h;
        n = 4;
    } else {
        n += arc(out + n, x + w - r, y + r, r, -YG_PI / 2, 0.0, seg);              /* sus-dreapta */
        n += arc(out + n, x + w - r, y + h - r, r, 0.0, YG_PI / 2, seg);           /* jos-dreapta */
        n += arc(out + n, x + r, y + h - r, r, YG_PI / 2, YG_PI, seg);             /* jos-stanga */
        n += arc(out + n, x + r, y + r, r, YG_PI, 1.5 * YG_PI, seg);               /* sus-stanga */
    }
    if(reverse){
        for(int i = 0; i < n / 2; i++){
            YgPoint t = out[i];
            out[i] = out[n - 1 - i];
            out[n - 1 - i] = t;
        }
    }
    return n;
}

static int corner_segments(float r){
    int n = (int)(r * 0.4f) + 3;
    return n > 64 ? 64 : n;
}

void yg_fill_rounded_rect(YgSurface *s, int x, int y, int w, int h, int r, YgColor c){
    if(w <= 0 || h <= 0){
        return;
    }
    int rmax = (w < h ? w : h) / 2;
    if(r > rmax) r = rmax;
    if(r <= 0){
        yg_fill_rect(s, x, y, w, h, c);
        return;
    }
    /* Interiorul fara colturi: trei dreptunghiuri fara suprapuneri (alfa < 255 nu se aplica de doua ori) */
    yg_fill_rect(s, x, y + r, w, h - 2 * r, c);
    yg_fill_rect(s, x + r, y, w - 2 * r, r, c);
    yg_fill_rect(s, x + r, y + h - r, w - 2 * r, r, c);
    /* Colturile: sectoare de un sfert de cerc, cu antialias; muchiile drepte cad pe granite de pixel */
    int seg = corner_segments((float)r);
    YgPoint pts[64 + 2];
    const float cx[4] = {(float)(x + r), (float)(x + w - r), (float)(x + w - r), (float)(x + r)};
    const float cy[4] = {(float)(y + r), (float)(y + r), (float)(y + h - r), (float)(y + h - r)};
    for(int k = 0; k < 4; k++){
        double a0 = YG_PI + k * (YG_PI / 2);
        pts[0].x = cx[k];
        pts[0].y = cy[k];
        int n = 1 + arc(pts + 1, cx[k], cy[k], (float)r, a0, a0 + YG_PI / 2, seg);
        yg_fill_polygon(s, pts, n, c);
    }
}

void yg_draw_rounded_rect(YgSurface *s, int x, int y, int w, int h, int r, int t, YgColor c){
    if(w <= 0 || h <= 0 || t <= 0){
        return;
    }
    int rmax = (w < h ? w : h) / 2;
    if(r > rmax) r = rmax;
    if(r < 0) r = 0;
    if(t * 2 >= w || t * 2 >= h){
        yg_fill_rounded_rect(s, x, y, w, h, r, c);
        return;
    }
    int seg = corner_segments((float)r);
    YgPoint *pts = malloc(sizeof(YgPoint) * (size_t)(seg + 1) * 8 + 8 * sizeof(YgPoint));
    if(!pts){
        return;
    }
    int counts[2];
    float ri = (float)(r - t);
    counts[0] = rrect_contour(pts, (float)x, (float)y, (float)w, (float)h, (float)r, seg, 0);
    counts[1] = rrect_contour(pts + counts[0], (float)(x + t), (float)(y + t), (float)(w - 2 * t), (float)(h - 2 * t),
                              ri > 0.0f ? ri : 0.0f, seg, 1);
    yg_fill_path(s, pts, counts, 2, c);
    free(pts);
}

void yg_draw_line_aa(YgSurface *s, float x0, float y0, float x1, float y1, float width, YgColor c){
    float dx = x1 - x0, dy = y1 - y0;
    float len = (float)sqrt((double)(dx * dx + dy * dy));
    if(len < 1e-6f || width <= 0.0f){
        return;
    }
    float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
    YgPoint q[4] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    yg_fill_polygon(s, q, 4, c);
}
