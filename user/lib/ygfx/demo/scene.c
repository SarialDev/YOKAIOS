/* gfxdemo: o scena care foloseste tot ce are ygfx, desenata intr-o zona (ecranul intreg sau o jumatate) cu o tema */
#include "scene.h"
#include <stdio.h>
#include <string.h>

/* coordonate relative la zona, scalate dupa 1024 x 768 */
typedef struct {
    YgSurface *s;
    const YgTheme *t;
    YgFont *reg, *bold;
    int x0, y0;
    float k;
} Ctx;

static int X(const Ctx *c, float v){ return c->x0 + (int)(v * c->k + 0.5f); }
static int Y(const Ctx *c, float v){ return c->y0 + (int)(v * c->k + 0.5f); }
static int L(const Ctx *c, float v){ return (int)(v * c->k + 0.5f); }

static void button(const Ctx *c, float x, float y, float w, float h, const char *label, int primary){
    YgRect r = yg_rect(X(c, x), Y(c, y), L(c, w), L(c, h));
    int rad = L(c, 10);
    if(primary){
        yg_fill_rounded_rect(c->s, r.x, r.y + L(c, 3), r.w, r.h, rad, c->t->shadow);
        yg_fill_rounded_rect(c->s, r.x, r.y, r.w, r.h, rad, c->t->accent);
        yg_draw_text_centered(c->s, c->bold, 20 * c->k, r, label, c->t->accent_text);
    } else {
        yg_fill_rounded_rect(c->s, r.x, r.y, r.w, r.h, rad, c->t->surface);
        yg_draw_rounded_rect(c->s, r.x, r.y, r.w, r.h, rad, L(c, 2) > 0 ? L(c, 2) : 1, c->t->border);
        yg_draw_text_centered(c->s, c->bold, 20 * c->k, r, label, c->t->text);
    }
}

static YgRect panel(const Ctx *c, float x, float y, float w, float h, const char *title){
    YgRect r = yg_rect(X(c, x), Y(c, y), L(c, w), L(c, h));
    int rad = L(c, 16);
    yg_fill_rounded_rect(c->s, r.x + L(c, 6), r.y + L(c, 8), r.w, r.h, rad, c->t->shadow);
    yg_fill_rounded_rect(c->s, r.x, r.y, r.w, r.h, rad, c->t->surface);
    yg_draw_rounded_rect(c->s, r.x, r.y, r.w, r.h, rad, 1, c->t->border);
    yg_draw_text(c->s, c->bold, 20 * c->k, r.x + L(c, 20), r.y + L(c, 34), title, c->t->text);
    yg_fill_rect(c->s, r.x + L(c, 20), r.y + L(c, 46), L(c, 40), L(c, 3) > 0 ? L(c, 3) : 1, c->t->accent);
    return r;
}

static void primitives(const Ctx *c, YgRect p){
    int ox = p.x + L(c, 20), oy = p.y + L(c, 66);
    /* evantai de linii Bresenham */
    for(int i = 0; i <= 8; i++){
        yg_draw_line(c->s, ox, oy + L(c, 110), ox + L(c, 130), oy + L(c, 110) - L(c, (float)i * 13.75f),
                     yg_mix(c->t->text_muted, c->t->accent, (uint8_t)(i * 31)));
    }
    /* linii groase cu antialias */
    for(int i = 0; i < 4; i++){
        float x = (float)(ox + L(c, 160)) + (float)i * 22 * c->k;
        yg_draw_line_aa(c->s, x, (float)(oy + L(c, 110)), x + 40 * c->k, (float)oy, (1.0f + i * 1.5f) * c->k, c->t->text);
    }
    /* cercuri */
    float cx = (float)(ox + L(c, 330)), cy = (float)(oy + L(c, 55));
    yg_fill_circle(c->s, cx, cy, 48 * c->k, c->t->accent);
    yg_draw_circle(c->s, cx, cy, 48 * c->k, 5 * c->k, c->t->text);
    yg_fill_circle(c->s, cx, cy, 16 * c->k, c->t->surface);
    /* dreptunghiuri: contur, rotunjit cu contur, gradient */
    int ry = oy + L(c, 140);
    yg_draw_rect(c->s, ox, ry, L(c, 110), L(c, 70), L(c, 3) > 0 ? L(c, 3) : 1, c->t->text_muted);
    yg_fill_rect(c->s, ox + L(c, 18), ry + L(c, 18), L(c, 74), L(c, 34), c->t->surface_alt);
    yg_fill_rounded_rect(c->s, ox + L(c, 130), ry, L(c, 120), L(c, 70), L(c, 22), c->t->surface_alt);
    yg_draw_rounded_rect(c->s, ox + L(c, 130), ry, L(c, 120), L(c, 70), L(c, 22), L(c, 4) > 0 ? L(c, 4) : 1, c->t->accent);
    yg_fill_gradient_v(c->s, ox + L(c, 270), ry, L(c, 140), L(c, 70), c->t->accent, c->t->surface);
    yg_draw_rect(c->s, ox + L(c, 270), ry, L(c, 140), L(c, 70), 1, c->t->border);
}

/* Sprite cu alfa radial (opac in centru, transparent la margine) */
static YgSurface *glow_sprite(int d, YgColor col){
    YgSurface *g = yg_surface_new(d, d);
    if(!g){
        return NULL;
    }
    float r = d / 2.0f;
    for(int y = 0; y < d; y++){
        for(int x = 0; x < d; x++){
            float dx = (x + 0.5f - r) / r, dy = (y + 0.5f - r) / r;
            float q = 1.0f - (dx * dx + dy * dy);
            int a = q <= 0.0f ? 0 : (int)(q * q * 255.0f);
            g->pixels[y * g->stride + x] = YG_WITH_ALPHA(col, a);
        }
    }
    return g;
}

static void alpha_clip(const Ctx *c, YgRect p){
    int ox = p.x + L(c, 20), oy = p.y + L(c, 66);
    /* tabla de sah sub obiectele translucide */
    int cell = L(c, 14) > 2 ? L(c, 14) : 2;
    YgRect board = yg_rect(ox, oy, cell * 14, cell * 9);
    for(int j = 0; j < 9; j++){
        for(int i = 0; i < 14; i++){
            yg_fill_rect(c->s, ox + i * cell, oy + j * cell, cell, cell, ((i + j) & 1) ? c->t->surface_alt : c->t->surface);
        }
    }
    yg_fill_circle(c->s, (float)(ox + cell * 4), (float)(oy + cell * 4), cell * 3.2f, YG_WITH_ALPHA(c->t->accent, 0x90));
    yg_fill_circle(c->s, (float)(ox + cell * 7), (float)(oy + cell * 5), cell * 3.2f, YG_WITH_ALPHA(c->t->text, 0x60));
    yg_fill_rounded_rect(c->s, ox + cell * 8, oy + cell, cell * 5, cell * 4, cell, YG_WITH_ALPHA(c->t->logo, 0x70));
    YgSurface *glow = glow_sprite(cell * 6, c->t->accent);
    if(glow){
        yg_blit_alpha(c->s, ox + cell * 8, oy + cell * 3, glow, NULL, 220);
        yg_surface_free(glow);
    }
    yg_draw_rect(c->s, board.x, board.y, board.w, board.h, 1, c->t->border);

    /* clipping: textul si liniile mai mari decat cutia se taie la marginea ei */
    YgRect box = yg_rect(board.x + board.w + L(c, 16), oy, p.x + p.w - L(c, 20) - (board.x + board.w + L(c, 16)), board.h);
    YgRect old = yg_set_clip(c->s, box);
    yg_clear(c->s, c->t->surface_alt);
    for(int i = -6; i < 12; i++){
        int lx = box.x + i * L(c, 18);
        yg_draw_line(c->s, lx, box.y + box.h, lx + box.h, box.y, c->t->border);
    }
    yg_draw_text(c->s, c->bold, 54 * c->k, box.x - L(c, 22), box.y + L(c, 62), "CLIP", c->t->accent);
    yg_draw_text(c->s, c->reg, 18 * c->k, box.x + L(c, 6), box.y + box.h - L(c, 10), "decupat la cutie", c->t->text);
    yg_fill_circle(c->s, (float)(box.x + box.w), (float)(box.y + box.h), 30 * c->k, YG_WITH_ALPHA(c->t->logo, 0xC0));
    c->s->clip = old;
    yg_draw_rect(c->s, box.x - 1, box.y - 1, box.w + 2, box.h + 2, 1, c->t->text_muted);

    int ty = oy + board.h + L(c, 34);
    yg_draw_text(c->s, c->reg, 17 * c->k, ox, ty, "Alfa per pixel, opacitate globala,", c->t->text_muted);
    yg_draw_text(c->s, c->reg, 17 * c->k, ox, ty + L(c, 24), "clip pe orice dreptunghi.", c->t->text_muted);
}

void scene_draw(YgSurface *s, const YgTheme *t, YgFont *reg, YgFont *bold, YgRect area){
    Ctx c = {s, t, reg, bold, area.x, area.y, 1.0f};
    float kx = area.w / 1024.0f, ky = area.h / 768.0f;
    c.k = kx < ky ? kx : ky;
    /* zona mai inalta / mai lata decat 4:3: scena se centreaza */
    c.x0 = area.x + (area.w - (int)(1024 * c.k)) / 2;
    c.y0 = area.y + (area.h - (int)(768 * c.k)) / 2;
    YgRect old = yg_set_clip(s, area);
    yg_clear(s, t->background);

    /* bara de sus */
    yg_fill_rect(s, area.x, c.y0, area.w, L(&c, 56), t->surface);
    yg_fill_rect(s, area.x, c.y0 + L(&c, 56), area.w, 1, t->border);
    yg_draw_logo(s, (float)X(&c, 34), (float)Y(&c, 28), 44 * c.k, t->logo);
    yg_draw_text(s, bold, 22 * c.k, X(&c, 66), Y(&c, 36), "YokaiOS", t->text);
    char label[32];
    snprintf(label, sizeof(label), "Tema %s", t == &yg_theme_dark ? "Dark" : "Light");
    int lw = yg_text_width(bold, 16 * c.k, label) + L(&c, 28);
    YgRect pill = yg_rect(area.x + area.w - lw - L(&c, 20), Y(&c, 14), lw, L(&c, 28));
    yg_fill_rounded_rect(s, pill.x, pill.y, pill.w, pill.h, pill.h / 2, t->accent);
    yg_draw_text_centered(s, bold, 16 * c.k, pill, label, t->accent_text);

    /* logo mare + titlu */
    yg_draw_logo(s, (float)X(&c, 190), (float)Y(&c, 230), 270 * c.k, t->logo);
    yg_draw_text(s, bold, 76 * c.k, X(&c, 360), Y(&c, 190), "YokaiOS", t->text);
    yg_draw_text(s, reg, 26 * c.k, X(&c, 364), Y(&c, 232), "ygfx \xC2\xB7 grafic\xC4\x83 pe CPU, f\xC4\x83r\xC4\x83 GPU", t->text_muted);
    yg_draw_text(s, reg, 20 * c.k, X(&c, 364), Y(&c, 266),
                 "Diacritice: \xC4\x83 \xC3\xA2 \xC3\xAE \xC8\x99 \xC8\x9B  \xC4\x82 \xC3\x82 \xC3\x8E \xC8\x98 \xC8\x9A", t->text_muted);
    button(&c, 364, 300, 170, 50, "Porne\xC8\x99te", 1);
    button(&c, 552, 300, 150, 50, "Set\xC4\x83ri", 0);

    YgRect p1 = panel(&c, 32, 400, 460, 320, "Primitive");
    primitives(&c, p1);
    YgRect p2 = panel(&c, 524, 400, 468, 320, "Alfa \xC8\x99i clipping");
    alpha_clip(&c, p2);

    YgCacheStats st = yg_font_cache_stats(reg);
    YgCacheStats sb = yg_font_cache_stats(bold);
    char line[128];
    snprintf(line, sizeof(line), "cache glife: %u + %u glife, %llu hit / %llu miss", st.glyphs, sb.glyphs,
             (unsigned long long)(st.hits + sb.hits), (unsigned long long)(st.misses + sb.misses));
    yg_draw_text(s, reg, 14 * c.k, X(&c, 32), Y(&c, 752), line, t->text_muted);
    s->clip = old;
}
