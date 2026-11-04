/* ypanel: desenul barei si localizarea elementelor (Start, aplicatii ancorate, task-uri, spatiu de lucru, ceas) */
#include "panel.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define START_W   46
#define START_H   32
#define PIN_W     38
#define PIN_GAP   2
#define TASK_MAX  172
#define TASK_MIN  28
#define TASK_GAP  4
#define CLOCK_W   58
#define WS_W      76
#define MARGIN    8

YgRect bar_start_rect(void){
    return yg_rect(P.sw / 2 - START_W / 2, (PANEL_H - START_H) / 2 + 1, START_W, START_H);
}

static YgRect pin_rect(int i){
    return yg_rect(MARGIN + i * (PIN_W + PIN_GAP), (PANEL_H - START_H) / 2 + 1, PIN_W, START_H);
}

/* Cate aplicatii ancorate incap fara sa ajunga la butonul Start */
static int pins_shown(void){
    int n = 0;
    YgRect st = bar_start_rect();
    while(n < P.npins && pin_rect(n).x + PIN_W <= st.x - MARGIN){
        n++;
    }
    return n;
}

static YgRect clock_rect(void){
    return yg_rect(P.sw - MARGIN - CLOCK_W, 1, CLOCK_W, PANEL_H - 1);
}

static YgRect ws_rect(void){
    YgRect c = clock_rect();
    return yg_rect(c.x - WS_W - 4, 1, WS_W, PANEL_H - 1);
}

/* Zona task-urilor: de dupa Start pana la indicatorul de spatiu */
static YgRect task_rect(int i){
    YgRect st = bar_start_rect();
    int x0 = st.x + st.w + 14;
    int x1 = ws_rect().x - 10;
    int n = P.ntasks > 0 ? P.ntasks : 1;
    int bw = (x1 - x0 - (n - 1) * TASK_GAP) / n;
    if(bw > TASK_MAX) bw = TASK_MAX;
    if(bw < TASK_MIN) bw = TASK_MIN;
    return yg_rect(x0 + i * (bw + TASK_GAP), (PANEL_H - START_H) / 2 + 1, bw, START_H);
}

static int task_visible(int i){
    return task_rect(i).x + task_rect(i).w <= ws_rect().x - 6;
}

void bar_hit(int x, int y, int *kind, int *idx){
    *kind = H_NONE;
    *idx = -1;
    if(yg_rect_contains(bar_start_rect(), x, y)){
        *kind = H_START;
        return;
    }
    int np = pins_shown();
    for(int i = 0; i < np; i++){
        if(yg_rect_contains(pin_rect(i), x, y)){
            *kind = H_PIN;
            *idx = P.pins[i];
            return;
        }
    }
    for(int i = 0; i < P.ntasks && task_visible(i); i++){
        if(yg_rect_contains(task_rect(i), x, y)){
            *kind = H_TASK;
            *idx = i;
            return;
        }
    }
}

void text_left(YgSurface *s, YgFont *f, int px, YgRect r, const char *str, YgColor col){
    if(!f){
        return;
    }
    YgRect old = yg_set_clip(s, yg_rect_intersect(s->clip, r));
    int asc, desc, gap;
    yg_font_vmetrics(f, (float)px, &asc, &desc, &gap);
    yg_draw_text(s, f, (float)px, r.x, r.y + (r.h + asc + desc) / 2, str, col);
    yg_set_clip(s, old);
}

/* Titlul taiat cu "..." ca sa incapa in maxw pixeli */
static void fit_text(char *out, size_t cap, int px, const char *in, int maxw){
    snprintf(out, cap, "%s", in);
    if(!P.font || yg_text_width(P.font, (float)px, out) <= maxw){
        return;
    }
    char tmp[48];
    for(size_t cut = strlen(in); cut > 0; cut--){
        if(((unsigned char)in[cut] & 0xC0) == 0x80){
            continue;               /* nu taia in mijlocul unui caracter UTF-8 */
        }
        snprintf(tmp, sizeof(tmp), "%.*s...", (int)cut, in);
        if(yg_text_width(P.font, (float)px, tmp) <= maxw){
            snprintf(out, cap, "%s", tmp);
            return;
        }
    }
    snprintf(out, cap, "...");
}

void draw_icon(YgSurface *s, int x, int y, int size, const char *name, int on_accent){
    const YgTheme *t = P.th;
    YgColor bg = on_accent ? t->accent_text : t->accent;
    YgColor fg = on_accent ? t->accent : t->accent_text;
    yg_fill_rounded_rect(s, x, y, size, size, size / 4, bg);
    if(P.bold){
        char ch[2] = {name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0};
        yg_draw_text_centered(s, P.bold, (float)(size * 5 / 9), yg_rect(x, y - 1, size, size), ch, fg);
    }
}

static YgColor hot(YgColor base, int is_hot){
    return is_hot ? yg_mix(base, P.th->text, 36) : base;
}

static void bar_update_clock_text(char *out, size_t cap){
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

int bar_update_clock(void){
    char buf[8];
    bar_update_clock_text(buf, sizeof(buf));
    if(!strcmp(buf, P.clock)){
        return 0;
    }
    memcpy(P.clock, buf, sizeof(P.clock));
    return 1;
}

void bar_draw(void){
    const YgTheme *t = P.th;
    YgSurface *s = yw_window_surface(P.win);
    yg_fill_rect(s, 0, 0, P.sw, PANEL_H, t->surface_alt);
    yg_fill_rect(s, 0, 0, P.sw, 1, t->border);

    /* Start: logo Yokai in centru */
    YgRect st = bar_start_rect();
    int open = menu_is_open();
    int hs = P.hover_kind == H_START;
    yg_fill_rounded_rect(s, st.x, st.y, st.w, st.h, 6, open ? t->accent : hot(t->surface_alt, hs));
    yg_draw_logo(s, st.x + st.w / 2.0f, st.y + st.h / 2.0f, 26.0f, open ? t->accent_text : t->logo);

    /* aplicatii ancorate */
    int np = pins_shown();
    for(int i = 0; i < np; i++){
        YgRect r = pin_rect(i);
        const App *a = &P.apps[P.pins[i]];
        int h = P.hover_kind == H_PIN && P.hover_idx == P.pins[i];
        yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, hot(t->surface_alt, h));
        draw_icon(s, r.x + (r.w - 24) / 2, r.y + (r.h - 24) / 2, 24, a->name, 0);
    }

    /* ferestrele deschise */
    for(int i = 0; i < P.ntasks && task_visible(i); i++){
        const Task *k = &P.tasks[i];
        YgRect r = task_rect(i);
        int h = P.hover_kind == H_TASK && P.hover_idx == i;
        int foc = (k->flags & YW_TF_FOCUSED) != 0;
        int mini = (k->flags & YW_TF_MINIMIZED) != 0;
        int other = (k->flags & YW_TF_OTHER_WS) != 0;
        YgColor bg = foc ? t->accent : hot(mini || other ? t->surface : t->border, h);
        YgColor fg = foc ? t->accent_text : (mini || other ? t->text_muted : t->text);
        yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, foc ? hot(bg, h) : bg);
        if(r.w >= 60){
            char txt[48];
            fit_text(txt, sizeof(txt), 13, k->title, r.w - 20);
            text_left(s, P.font, 13, yg_rect(r.x + 10, r.y, r.w - 14, r.h), txt, fg);
        } else {
            char ch[2] = {k->title[0] ? k->title[0] : '?', 0};
            yg_draw_text_centered(s, P.bold ? P.bold : P.font, 14, r, ch, fg);
        }
        if(mini){
            yg_fill_rect(s, r.x + 10, r.y + r.h - 4, 8, 2, fg);        /* minimizata: liniuta */
        }
    }

    /* spatiul de lucru + modul */
    char wsb[24];
    snprintf(wsb, sizeof(wsb), "%d  %s", P.ws + 1, (P.tilemask >> P.ws) & 1 ? "TILE" : "FLOAT");
    yg_draw_text_centered(s, P.font, 12, ws_rect(), wsb, t->text_muted);

    /* ceas */
    yg_draw_text_centered(s, P.bold ? P.bold : P.font, 15, clock_rect(), P.clock, t->text);
}

void bar_redraw(void){
    bar_draw();
    yw_window_damage_all(P.win);
}
