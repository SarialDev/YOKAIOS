/* Managerul de ferestre (T10.3): decoratiile ferestrelor, panoul de jos si previzualizarea snap-ului, in stilul temei */
#include "comp.h"
#include <stdio.h>
#include <string.h>

static uint8_t scale8(uint8_t a, uint8_t op){
    return (uint8_t)((a * op + 127) / 255);
}

/* Butoanele stau in dreapta barei: inchidere (cel mai la dreapta), maximizare, minimizare */
YgRect wm_btn_rect(const Window *w, int zone){
    YgRect o = window_outer(w);
    int slot = zone == Z_CLOSE ? 0 : zone == Z_MAX ? 1 : 2;
    int x = o.x + o.w - 6 - CLOSE_SIZE - slot * (CLOSE_SIZE + WM_BTN_GAP);
    return yg_rect(x, o.y + (TITLE_H - CLOSE_SIZE) / 2, CLOSE_SIZE, CLOSE_SIZE);
}

static void draw_button(const Window *w, int zone, YgColor bar, YgColor fg, int hot, uint8_t op){
    YgRect b = wm_btn_rect(w, zone);
    yg_fill_rounded_rect(C.back, b.x, b.y, b.w, b.h, 4, YG_WITH_ALPHA(yg_mix(bar, fg, hot ? 120 : 55), op));
    YgColor g = YG_WITH_ALPHA(fg, op);
    float m = 4.5f;
    if(zone == Z_CLOSE){
        yg_draw_line_aa(C.back, b.x + m, b.y + m, b.x + b.w - m, b.y + b.h - m, 1.6f, g);
        yg_draw_line_aa(C.back, b.x + b.w - m, b.y + m, b.x + m, b.y + b.h - m, 1.6f, g);
    } else if(zone == Z_MAX){
        if(w->wm.snap == SNAP_MAX){
            /* restaurare: doua patrate suprapuse */
            yg_draw_rect(C.back, b.x + 6, b.y + 4, 6, 6, 1, g);
            yg_draw_rect(C.back, b.x + 4, b.y + 6, 6, 6, 1, g);
        } else {
            yg_draw_rect(C.back, b.x + 4, b.y + 4, 8, 8, 1, g);
            yg_fill_rect(C.back, b.x + 4, b.y + 4, 8, 2, g);
        }
    } else {
        yg_fill_rect(C.back, b.x + 4, b.y + b.h - 6, 8, 2, g);
    }
}

void wm_draw_decor(const Window *w, int focused, YgRect clip){
    const YgTheme *t = C.theme;
    uint8_t op = w->opacity;
    YgRect o = window_outer(w);
    YgRect tb = window_title(w);
    YgColor bar = focused ? t->accent : t->surface_alt;
    YgColor fg = focused ? t->accent_text : t->text_muted;
    YgRect old = yg_set_clip(C.back, yg_rect_intersect(clip, o));
    yg_fill_rect(C.back, tb.x, tb.y, tb.w, tb.h, YG_WITH_ALPHA(bar, op));
    YgColor bcol = focused ? t->accent : t->border;
    YgColor bc = YG_WITH_ALPHA(bcol, scale8(YG_A(bcol), op));
    yg_fill_rect(C.back, o.x, o.y + TITLE_H, BORDER, o.h - TITLE_H, bc);
    yg_fill_rect(C.back, o.x + o.w - BORDER, o.y + TITLE_H, BORDER, o.h - TITLE_H, bc);
    yg_fill_rect(C.back, o.x, o.y + o.h - BORDER, o.w, BORDER, bc);
    int widx = (int)(w - C.win);
    static const int zones[3] = {Z_MIN, Z_MAX, Z_CLOSE};
    for(int i = 0; i < 3; i++){
        draw_button(w, zones[i], bar, fg, C.wm.hot_win == widx && C.wm.hot_btn == zones[i], op);
    }
    if(C.font && w->title[0]){
        YgRect mb = wm_btn_rect(w, Z_MIN);
        YgRect text = yg_rect(tb.x + 10, tb.y, mb.x - tb.x - 16, tb.h);
        YgRect o2 = yg_set_clip(C.back, yg_rect_intersect(C.back->clip, text));
        int asc, desc, gap;
        yg_font_vmetrics(C.font, 14, &asc, &desc, &gap);
        int base = tb.y + (TITLE_H + asc + desc) / 2;
        yg_draw_text(C.back, C.font, 14, text.x, base, w->title, YG_WITH_ALPHA(fg, op));
        yg_set_clip(C.back, o2);
    }
    yg_set_clip(C.back, old);
}

/* ---- panoul de jos ---- */

YgRect wm_panel_rect(void){
    return yg_rect(0, C.sh - WM_PANEL_H, C.sw, WM_PANEL_H);
}

YgRect wm_tab_rect(int ws){
    return yg_rect(6 + ws * 32, C.sh - WM_PANEL_H + 3, 28, WM_PANEL_H - 6);
}

YgRect wm_mode_rect(void){
    return yg_rect(6 + WM_NWS * 32 + 6, C.sh - WM_PANEL_H + 3, 64, WM_PANEL_H - 6);
}

void wm_draw_panel(YgRect clip){
    YgRect p = wm_panel_rect();
    if(yg_rect_empty(yg_rect_intersect(p, clip))){
        return;
    }
    const YgTheme *t = C.theme;
    YgRect old = yg_set_clip(C.back, yg_rect_intersect(clip, p));
    yg_fill_rect(C.back, p.x, p.y, p.w, p.h, t->surface_alt);
    yg_fill_rect(C.back, p.x, p.y, p.w, 1, t->border);
    for(int i = 0; i < WM_NWS; i++){
        YgRect r = wm_tab_rect(i);
        int cur = i == C.wm.cur;
        if(cur){
            yg_fill_rounded_rect(C.back, r.x, r.y, r.w, r.h, 4, t->accent);
        }
        if(C.font){
            char num[2] = {(char)('1' + i), 0};
            YgRect tr = yg_rect(r.x, r.y - 2, r.w, r.h);
            yg_draw_text_centered(C.back, C.font, 13, tr, num, cur ? t->accent_text : t->text_muted);
        }
        int n = wm_ws_count(i);
        if(n){
            /* un punct pe fereastra (cel mult 5) sub cifra */
            int dots = n > 5 ? 5 : n;
            int x0 = r.x + (r.w - (dots * 4 - 2)) / 2;
            for(int d = 0; d < dots; d++){
                yg_fill_rect(C.back, x0 + d * 4, r.y + r.h - 4, 2, 2, cur ? t->accent_text : t->text_muted);
            }
        }
    }
    YgRect mr = wm_mode_rect();
    yg_fill_rounded_rect(C.back, mr.x, mr.y, mr.w, mr.h, 4, t->surface);
    if(C.font){
        YgRect tr = yg_rect(mr.x, mr.y - 2, mr.w, mr.h);
        yg_draw_text_centered(C.back, C.font, 12, tr, wm_mode() == WM_TILE ? "TILE" : "FLOAT", t->text);
        if(C.focus >= 0 && C.win[C.focus].title[0]){
            int x = mr.x + mr.w + 14;
            YgRect text = yg_rect(x, p.y, p.w - x - 10, p.h);
            YgRect o2 = yg_set_clip(C.back, yg_rect_intersect(C.back->clip, text));
            int asc, desc, gap;
            yg_font_vmetrics(C.font, 13, &asc, &desc, &gap);
            yg_draw_text(C.back, C.font, 13, x, p.y + (p.h + asc + desc) / 2, C.win[C.focus].title, t->text_muted);
            yg_set_clip(C.back, o2);
        }
    }
    yg_set_clip(C.back, old);
}

/* Previzualizarea snap-ului / tintei de schimb: umplere transparenta + contur */
void wm_draw_preview(YgRect clip){
    if(!C.wm.has_preview){
        return;
    }
    YgRect r = C.wm.preview;
    if(yg_rect_empty(yg_rect_intersect(r, clip))){
        return;
    }
    YgRect old = yg_set_clip(C.back, yg_rect_intersect(clip, r));
    yg_fill_rect(C.back, r.x, r.y, r.w, r.h, YG_WITH_ALPHA(C.theme->accent, 70));
    yg_draw_rect(C.back, r.x, r.y, r.w, r.h, 2, C.theme->accent);
    yg_set_clip(C.back, old);
}
