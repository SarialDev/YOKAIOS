#ifndef YUI_H
#define YUI_H
/* yui (T10.5): ajutoare de desen pentru aplicatiile GUI (yfiles, ysettings): text aliniat si decupat, butoane, campuri,
 * bara de derulare, dialoguri. Numai antet (static inline), peste ygfx; fara stare globala in afara structurii YUi. */
#include "ygfx.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    YgSurface *s;
    YgFont *f, *b;          /* normal, aldin */
    const YgTheme *t;
} YUi;

enum { YUI_LEFT = 0, YUI_CENTER = 1, YUI_RIGHT = 2 };

static inline int yui_in(YgRect r, int x, int y){
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Text pe un rand, centrat vertical in r, decupat la r; align: YUI_LEFT / CENTER / RIGHT (padding orizontal 6) */
static inline void yui_text(YUi *u, float px, YgRect r, const char *str, YgColor col, int align, int bold){
    YgFont *f = bold && u->b ? u->b : u->f;
    if(!f || !str || !*str){
        return;
    }
    int asc, desc, gap;
    yg_font_vmetrics(f, px, &asc, &desc, &gap);
    int w = yg_text_width(f, px, str);
    int x = r.x + 6;
    if(align == YUI_CENTER){
        x = r.x + (r.w - w) / 2;
    } else if(align == YUI_RIGHT){
        x = r.x + r.w - 6 - w;
    }
    int base = r.y + (r.h + asc + desc) / 2;
    YgRect old = yg_set_clip(u->s, r);
    yg_draw_text(u->s, f, px, x, base, str, col);
    u->s->clip = old;
}

/* Scurteaza str cu "..." ca sa incapa in maxw pixeli (la px); out are cap octeti */
static inline void yui_fit(YUi *u, float px, const char *str, int maxw, char *out, size_t cap){
    snprintf(out, cap, "%s", str);
    if(yg_text_width(u->f, px, out) <= maxw){
        return;
    }
    size_t n = strlen(out);
    while(n > 1){
        n--;
        while(n > 1 && ((unsigned char)out[n] & 0xC0) == 0x80){
            n--;
        }
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.*s...", (int)n, str);
        if(yg_text_width(u->f, px, tmp) <= maxw){
            snprintf(out, cap, "%s", tmp);
            return;
        }
    }
    snprintf(out, cap, "...");
}

/* state: 0 normal, 1 deasupra (hover), 2 activ (apasat / comutat pe); enabled = 0: estompat */
static inline void yui_button(YUi *u, YgRect r, const char *label, int state, int enabled){
    const YgTheme *t = u->t;
    YgColor bg = state == 2 ? t->accent : state == 1 ? yg_mix(t->surface_alt, t->text, 28) : t->surface_alt;
    YgColor fg = state == 2 ? t->accent_text : t->text;
    if(!enabled){
        fg = t->text_muted;
        bg = t->surface_alt;
    }
    yg_fill_rounded_rect(u->s, r.x, r.y, r.w, r.h, 5, bg);
    yg_draw_rounded_rect(u->s, r.x, r.y, r.w, r.h, 5, 1, t->border);
    yui_text(u, 13, r, label, fg, YUI_CENTER, 0);
}

/* Camp de text cu cursor (caret = pozitia in octeti) */
static inline void yui_field(YUi *u, YgRect r, const char *text, int caret, int focused){
    const YgTheme *t = u->t;
    yg_fill_rounded_rect(u->s, r.x, r.y, r.w, r.h, 4, t->surface_alt);
    yg_draw_rounded_rect(u->s, r.x, r.y, r.w, r.h, 4, 1, focused ? t->accent : t->border);
    yui_text(u, 13, r, text, t->text, YUI_LEFT, 0);
    if(focused){
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.*s", caret, text);
        int cx = r.x + 6 + yg_text_width(u->f, 13, tmp);
        yg_fill_rect(u->s, cx, r.y + 4, 1, r.h - 8, t->text);
    }
}

/* Bara de derulare verticala in track (total / vizibile / decalaj in unitati oarecare) */
static inline void yui_scrollbar(YUi *u, YgRect track, int total, int visible, int offset){
    if(total <= visible || visible <= 0){
        return;
    }
    int th = track.h * visible / total;
    if(th < 16){
        th = 16;
    }
    int ty = track.y + (track.h - th) * offset / (total - visible);
    yg_fill_rounded_rect(u->s, track.x + 1, ty, track.w - 2, th, 3, yg_mix(u->t->surface, u->t->text_muted, 150));
}

/* Dialog modal centrat in suprafata: umbreste fundalul, deseneaza panoul si intoarce dreptunghiul lui */
static inline YgRect yui_dialog(YUi *u, int w, int h, const char *title){
    const YgTheme *t = u->t;
    YgSurface *s = u->s;
    yg_fill_rect(s, 0, 0, s->width, s->height, YG_ARGB(110, 0, 0, 0));
    YgRect r = yg_rect((s->width - w) / 2, (s->height - h) / 2, w, h);
    yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, t->surface);
    yg_draw_rounded_rect(s, r.x, r.y, r.w, r.h, 8, 1, t->border);
    yui_text(u, 15, yg_rect(r.x + 8, r.y + 6, r.w - 16, 28), title, t->text, YUI_LEFT, 1);
    return r;
}
#endif
