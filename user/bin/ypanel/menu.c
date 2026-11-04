/* ypanel: meniul Start. Un popup deasupra butonului Start cu cautare (se scrie direct) si lista aplicatiilor din
 * /etc/ypanel.conf; Sus / Jos aleg, Enter porneste, Esc inchide; mouse: trecerea alege, clic porneste. Pierderea focusului
 * (clic in alta parte) inchide meniul. */
#include "panel.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/yinput.h>

#define MENU_W    320
#define MENU_H    460
#define SEARCH_Y  10
#define SEARCH_H  32
#define LIST_Y    54
#define ROW_H     36
#define PAD       8

static YwWindow *menu;
static uint32_t menu_id;
static int menu_h;
static char query[48];
static int sel, scroll;
static int flt[MAX_APPS], nflt;

int menu_is_open(void){
    return menu != NULL;
}

uint32_t menu_window_id(void){
    return menu ? menu_id : 0;
}

int menu_visible_count(void){
    return menu ? nflt : 0;
}

static int rows_visible(void){
    return (menu_h - LIST_Y - PAD) / ROW_H;
}

static int starts_ci(const char *s, const char *q){
    for(; *q; s++, q++){
        if(tolower((unsigned char)*s) != tolower((unsigned char)*q)){
            return 0;
        }
    }
    return 1;
}

static int contains_ci(const char *s, const char *q){
    for(; *s; s++){
        if(starts_ci(s, q)){
            return 1;
        }
    }
    return 0;
}

/* Cele care incep cu textul cautat, apoi cele care il contin; fara text: toate, in ordinea din fisier */
static void refilter(void){
    nflt = 0;
    for(int pass = 0; pass < 2; pass++){
        for(int i = 0; i < P.napps; i++){
            int m = !query[0] || (pass == 0 ? starts_ci(P.apps[i].name, query)
                                            : (!starts_ci(P.apps[i].name, query) && contains_ci(P.apps[i].name, query)));
            if(m && (pass == 0 || query[0])){
                flt[nflt++] = i;
            }
        }
    }
    if(sel >= nflt){
        sel = nflt ? nflt - 1 : 0;
    }
    if(sel < 0){
        sel = 0;
    }
    scroll = 0;
    if(sel >= rows_visible()){
        scroll = sel - rows_visible() + 1;
    }
}

static void draw(void){
    const YgTheme *t = P.th;
    YgSurface *s = yw_window_surface(menu);
    yg_clear(s, t->surface);
    yg_draw_rect(s, 0, 0, MENU_W, menu_h, 1, t->border);

    YgRect sr = yg_rect(PAD + 2, SEARCH_Y, MENU_W - 2 * PAD - 4, SEARCH_H);
    yg_fill_rounded_rect(s, sr.x, sr.y, sr.w, sr.h, 6, t->surface_alt);
    if(query[0]){
        text_left(s, P.font, 15, yg_rect(sr.x + 10, sr.y, sr.w - 20, sr.h), query, t->text);
        int w = P.font ? yg_text_width(P.font, 15, query) : 0;
        yg_fill_rect(s, sr.x + 11 + w, sr.y + 7, 1, sr.h - 14, t->accent);
    } else {
        text_left(s, P.font, 15, yg_rect(sr.x + 10, sr.y, sr.w - 20, sr.h), "Cauta aplicatii...", t->text_muted);
        yg_fill_rect(s, sr.x + 10, sr.y + 7, 1, sr.h - 14, t->accent);
    }

    int vis = rows_visible();
    for(int r = 0; r < vis && scroll + r < nflt; r++){
        int ai = flt[scroll + r];
        int is_sel = scroll + r == sel;
        YgRect row = yg_rect(PAD, LIST_Y + r * ROW_H, MENU_W - 2 * PAD, ROW_H - 2);
        if(is_sel){
            yg_fill_rounded_rect(s, row.x, row.y, row.w, row.h, 6, t->accent);
        }
        draw_icon(s, row.x + 6, row.y + (row.h - 24) / 2, 24, P.apps[ai].name, is_sel);
        text_left(s, P.font, 15, yg_rect(row.x + 42, row.y, row.w - 50, row.h), P.apps[ai].name,
                  is_sel ? t->accent_text : t->text);
    }
    if(!nflt){
        yg_draw_text_centered(s, P.font, 14, yg_rect(0, LIST_Y, MENU_W, 60), "Nicio aplicatie", t->text_muted);
    }
    yw_window_damage_all(menu);
}

void menu_close(void){
    if(!menu){
        return;
    }
    YwWindow *w = menu;
    menu = NULL;
    menu_id = 0;
    yw_window_destroy(w);
    bar_redraw();                   /* butonul Start nu mai e apasat */
}

static void menu_open(void){
    if(menu){
        return;
    }
    int h = MENU_H;
    int room = P.sh - PANEL_H - 12;
    if(h > room){
        h = room;
    }
    if(h < LIST_Y + 2 * ROW_H + PAD){
        h = LIST_Y + 2 * ROW_H + PAD;
    }
    int x = P.sw / 2 - MENU_W / 2;
    if(x > P.sw - MENU_W - 8) x = P.sw - MENU_W - 8;
    if(x < 8) x = 8;
    int y = P.sh - PANEL_H - h - 6;
    menu = yw_window_new_role(P.c, MENU_W, h, "Start", YW_ROLE_POPUP, x, y);
    if(!menu){
        return;
    }
    menu_id = yw_window_id(menu);
    menu_h = h;
    query[0] = 0;
    sel = scroll = 0;
    refilter();
    draw();
    bar_redraw();
}

void menu_toggle(void){
    if(menu){
        menu_close();
    } else {
        menu_open();
    }
}

static void launch_sel(void){
    if(sel >= 0 && sel < nflt){
        app_launch(P.apps[flt[sel]].cmd);
        menu_close();
    }
}

static void move_sel(int d){
    if(!nflt){
        return;
    }
    sel += d;
    if(sel < 0) sel = 0;
    if(sel >= nflt) sel = nflt - 1;
    if(sel < scroll) scroll = sel;
    if(sel >= scroll + rows_visible()) scroll = sel - rows_visible() + 1;
}

static int row_at(int y){
    if(y < LIST_Y){
        return -1;
    }
    int r = (y - LIST_Y) / ROW_H;
    if(r >= rows_visible() || scroll + r >= nflt){
        return -1;
    }
    return scroll + r;
}

void menu_event(const YwEvent *ev){
    if(!menu){
        return;
    }
    switch(ev->type){
    case YW_EV_FOCUS:
        if(!ev->focus){
            menu_close();           /* clic in alta parte */
        }
        break;
    case YW_EV_CLOSE:
        menu_close();
        break;
    case YW_EV_KEY:
        if(!ev->pressed){
            break;
        }
        if(ev->keycode == YK_KEY_ESC){
            menu_close();
        } else if(ev->keycode == YK_KEY_ENTER){
            launch_sel();
        } else if(ev->keycode == YK_KEY_UP){
            move_sel(-1);
            draw();
        } else if(ev->keycode == YK_KEY_DOWN){
            move_sel(1);
            draw();
        } else if(ev->keycode == YK_KEY_BACKSPACE){
            size_t n = strlen(query);
            if(n){
                query[n - 1] = 0;
                refilter();
                draw();
            }
        } else if(ev->ascii >= 32 && ev->ascii < 127 && !(ev->modifiers & (YK_MOD_CTRL | YK_MOD_ALT))){
            size_t n = strlen(query);
            if(n + 1 < sizeof(query)){
                query[n] = (char)ev->ascii;
                query[n + 1] = 0;
                refilter();
                draw();
            }
        }
        break;
    case YW_EV_MOUSE: {
        int r = row_at(ev->y);
        if(ev->kind == YW_MOUSE_MOVE && r >= 0 && r != sel){
            sel = r;
            draw();
        } else if(ev->kind == YW_MOUSE_PRESS && (ev->buttons & YK_BTN_LEFT) && r >= 0){
            sel = r;
            launch_sel();
        } else if(ev->kind == YW_MOUSE_WHEEL && ev->wheel){
            move_sel(ev->wheel > 0 ? -1 : 1);
            draw();
        }
        break;
    }
    default:
        break;
    }
}
