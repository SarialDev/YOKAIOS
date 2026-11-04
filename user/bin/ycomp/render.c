/* ycomp: compunerea cu dreptunghiuri murdare intr-un back buffer din RAM, apoi copierea lor pe /dev/fb0 */
#include "comp.h"
#include <stdio.h>
#include <string.h>

/* Sageata 12x19: '#' contur negru, '.' alb */
static const char *const ARROW[] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#..........#",
    "#......#####",
    "#...#..#    ",
    "#..##..#    ",
    "#.#  #..#   ",
    "##   #..#   ",
    "      #..#  ",
    "      ####  ",
};
#define CUR_W 12
#define CUR_H 19

static int rect_overlap_or_touch(YgRect a, YgRect b){
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

static YgRect rect_union(YgRect a, YgRect b){
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return yg_rect(x0, y0, x1 - x0, y1 - y0);
}

static long area(YgRect r){
    return (long)r.w * r.h;
}

void damage(YgRect r){
    r = yg_rect_intersect(r, yg_rect(0, 0, C.sw, C.sh));
    if(yg_rect_empty(r)){
        return;
    }
    /* Unire cu tot ce se atinge (repetat: zona marita poate atinge altele) */
    for(int i = 0; i < C.ndirty;){
        if(rect_overlap_or_touch(r, C.dirty[i])){
            r = rect_union(r, C.dirty[i]);
            C.dirty[i] = C.dirty[--C.ndirty];
            i = 0;
        } else {
            i++;
        }
    }
    if(C.ndirty == MAX_DIRTY){
        int best = 0;
        long grow = -1;
        for(int i = 0; i < C.ndirty; i++){
            long g = area(rect_union(r, C.dirty[i])) - area(C.dirty[i]);
            if(grow < 0 || g < grow){
                grow = g;
                best = i;
            }
        }
        r = rect_union(r, C.dirty[best]);
        C.dirty[best] = C.dirty[--C.ndirty];
    }
    C.dirty[C.ndirty++] = r;
    request_frame();
}

/* Panoul si popup-urile (T10.4) nu au decoratii: cadrul e chiar continutul, bara de titlu e goala */
YgRect window_outer(const Window *w){
    if(w->role != YW_ROLE_NORMAL){
        return yg_rect(w->x, w->y, w->w, w->h);
    }
    return yg_rect(w->x, w->y, w->w + 2 * BORDER, w->h + TITLE_H + BORDER);
}

YgRect window_content(const Window *w){
    if(w->role != YW_ROLE_NORMAL){
        return yg_rect(w->x, w->y, w->w, w->h);
    }
    return yg_rect(w->x + BORDER, w->y + TITLE_H, w->w, w->h);
}

YgRect window_title(const Window *w){
    if(w->role != YW_ROLE_NORMAL){
        return yg_rect(w->x, w->y, 0, 0);
    }
    return yg_rect(w->x, w->y, w->w + 2 * BORDER, TITLE_H);
}

YgRect window_close_box(const Window *w){
    return wm_btn_rect(w, Z_CLOSE);
}

void damage_window(const Window *w){
    damage(window_outer(w));
}

static YgRect cursor_rect(void){
    return yg_rect(C.cx, C.cy, CUR_W, CUR_H);
}

void damage_cursor(void){
    damage(cursor_rect());
}

static void draw_wallpaper(void){
    const YgTheme *t = C.theme;
    YgSurface *s = C.wall;
    YgColor top = t->background;
    YgColor bottom = yg_mix(t->background, t->surface, 160);
    yg_fill_gradient_v(s, 0, 0, C.sw, C.sh, top, bottom);
    float size = (float)(C.sh < C.sw ? C.sh : C.sw) * 0.32f;
    yg_draw_logo(s, C.sw / 2.0f, C.sh / 2.0f - size * 0.1f, size, YG_WITH_ALPHA(t->logo, 70));
    if(C.font){
        YgRect r = yg_rect(0, (int)(C.sh / 2.0f + size * 0.5f), C.sw, 40);
        yg_draw_text_centered(s, C.font, 22, r, "YokaiOS", YG_WITH_ALPHA(t->text_muted, 160));
    }
}

static void make_cursor(void){
    yg_clear(C.cursor, 0);
    for(int y = 0; y < CUR_H; y++){
        for(int x = 0; x < CUR_W; x++){
            char ch = ARROW[y][x];
            if(ch == '#'){
                yg_set_pixel(C.cursor, x, y, YG_RGB(0, 0, 0));
            } else if(ch == '.'){
                yg_set_pixel(C.cursor, x, y, YG_RGB(255, 255, 255));
            }
        }
    }
}

int render_init(const char *theme){
    int err = yg_fb_open(&C.fb, NULL);
    if(err){
        return err;
    }
    C.sw = C.fb.width;
    C.sh = C.fb.height;
    C.theme = theme ? yg_theme_by_name(theme) : NULL;
    if(!C.theme){
        C.theme = &yg_theme_dark;
    }
    C.back = yg_surface_new(C.sw, C.sh);
    C.wall = yg_surface_new(C.sw, C.sh);
    C.cursor = yg_surface_new(CUR_W, CUR_H);
    if(!C.back || !C.wall || !C.cursor){
        return -12;
    }
    C.font = yg_font_load(YG_FONT_BOLD);
    draw_wallpaper();
    make_cursor();
    C.cx = C.sw / 2;
    C.cy = C.sh / 2;
    wm_init();
    damage(yg_rect(0, 0, C.sw, C.sh));
    return 0;
}

void render_fini(void){
    yg_fb_close(&C.fb);
    yg_surface_free(C.back);
    yg_surface_free(C.wall);
    yg_surface_free(C.cursor);
    if(C.font){
        yg_font_free(C.font);
    }
}

/* Continutul clientului (opac), decupat la clip; cu opacitate < 255 se amesteca peste ce e dedesubt. Daca buferul e mai mic
 * decat cadrul (clientul nu a raspuns inca la YW_EV_RESIZE), restul se umple cu culoarea suprafetei; daca e mai mare, se taie */
static void draw_content(const Window *w, YgRect clip){
    YgRect cr = window_content(w);
    YgRect r = yg_rect_intersect(cr, clip);
    if(yg_rect_empty(r)){
        return;
    }
    if(!w->pixels){
        yg_fill_rect(C.back, r.x, r.y, r.w, r.h, YG_WITH_ALPHA(C.theme->surface, w->opacity));
        return;
    }
    if(w->bw < w->w || w->bh < w->h){
        yg_fill_rect(C.back, r.x, r.y, r.w, r.h, YG_WITH_ALPHA(C.theme->surface, w->opacity));
    }
    r = yg_rect_intersect(r, yg_rect(cr.x, cr.y, w->bw, w->bh));
    for(int y = r.y; y < r.y + r.h; y++){
        const uint32_t *src = w->pixels + (size_t)(y - cr.y) * w->stride + (r.x - cr.x);
        uint32_t *dst = C.back->pixels + (size_t)y * C.back->stride + r.x;
        if(w->opacity == 255){
            for(int i = 0; i < r.w; i++){
                dst[i] = src[i] | 0xFF000000u;
            }
        } else {
            for(int i = 0; i < r.w; i++){
                dst[i] = yg_mix(dst[i], src[i] | 0xFF000000u, w->opacity);
            }
        }
    }
}

static void draw_window(const Window *w, int focused, YgRect clip){
    YgRect old = yg_set_clip(C.back, yg_rect_intersect(clip, window_outer(w)));
    if(w->role == YW_ROLE_NORMAL){
        wm_draw_decor(w, focused, C.back->clip);
    }
    draw_content(w, C.back->clip);
    yg_set_clip(C.back, old);
}

void compose_and_flip(void){
    for(int i = 0; i < C.ndirty; i++){
        YgRect d = C.dirty[i];
        yg_set_clip(C.back, d);
        yg_blit(C.back, d.x, d.y, C.wall, &d);
        for(int z = 0; z < C.nz; z++){
            const Window *w = &C.win[C.zorder[z]];
            if(wm_visible(w) && !yg_rect_empty(yg_rect_intersect(window_outer(w), d))){
                draw_window(w, C.zorder[z] == C.focus, d);
            }
        }
        wm_draw_panel(d);
        wm_draw_preview(d);
        YgRect cr = cursor_rect();
        if(!yg_rect_empty(yg_rect_intersect(cr, d))){
            yg_blit_alpha(C.back, cr.x, cr.y, C.cursor, NULL, 255);
        }
        C.pixels += (uint64_t)d.w * d.h;
    }
    yg_reset_clip(C.back);
    /* "Flip": doar zonele schimbate trec in memoria video */
    for(int i = 0; i < C.ndirty; i++){
        yg_fb_present(&C.fb, C.back, &C.dirty[i]);
    }
    C.ndirty = 0;
    C.frames++;
}

uint32_t back_pixel(int x, int y){
    if(x < 0 || y < 0 || x >= C.sw || y >= C.sh){
        return 0;
    }
    return C.back->pixels[(size_t)y * C.back->stride + x] & 0xFFFFFFu;
}
