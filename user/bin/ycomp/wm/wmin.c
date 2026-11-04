/* Managerul de ferestre (T10.3): scurtaturile de tastatura si interactiunea cu mouse-ul (mutare, redimensionare, snap,
 * schimbare de locuri in tiling, butoanele din bara de titlu, panoul de jos) */
#include "comp.h"
#include <string.h>
#include <sys/yinput.h>

/* Codurile logice ale tastelor (kernel/include/keycodes.h; yinput.h le da doar pe cateva) */
enum {
    K_1 = 2, K_4 = 5, K_TAB = 15, K_Q = 16, K_T = 20, K_ENTER = 28, K_F = 33, K_H = 35, K_L = 38, K_X = 45, K_M = 50,
    K_LALT = 55, K_RALT = 71, K_UP = 73, K_LEFT = 75, K_RIGHT = 76, K_DOWN = 78, K_LWIN = 82, K_RWIN = 83,
};

static int bit_get(int code){
    return code >= 0 && code < 128 && (C.wm.consumed[code >> 3] >> (code & 7)) & 1;
}

static void bit_set(int code, int v){
    if(code < 0 || code >= 128){
        return;
    }
    if(v){
        C.wm.consumed[code >> 3] |= (uint8_t)(1u << (code & 7));
    } else {
        C.wm.consumed[code >> 3] &= (uint8_t)~(1u << (code & 7));
    }
}

/* ---- tastatura ---- */

static void win_binding(const struct yk_input_event *e){
    int code = e->keycode;
    int shift = (e->modifiers & YK_MOD_SHIFT) != 0;
    int f = C.focus;
    if(code >= K_1 && code <= K_4){
        if(shift){
            if(f >= 0){
                wm_move_to_ws(f, code - K_1);
            }
        } else {
            wm_switch(code - K_1);
        }
        return;
    }
    switch(code){
    case K_T:
        wm_set_mode(WM_TILE);
        break;
    case K_F:
        wm_set_mode(WM_FLOAT);
        break;
    case K_Q:
        if(f >= 0) wm_close(f);
        break;
    case K_M:
        if(f >= 0) wm_minimize(f);
        break;
    case K_X:
        if(f >= 0) wm_toggle_max(f);
        break;
    case K_LEFT:
        if(f >= 0) wm_set_snap(f, C.win[f].wm.snap == SNAP_LEFT ? SNAP_NONE : SNAP_LEFT);
        break;
    case K_RIGHT:
        if(f >= 0) wm_set_snap(f, C.win[f].wm.snap == SNAP_RIGHT ? SNAP_NONE : SNAP_RIGHT);
        break;
    case K_UP:
        if(f >= 0) wm_set_snap(f, SNAP_MAX);
        break;
    case K_DOWN:
        if(f >= 0){
            if(C.win[f].wm.snap != SNAP_NONE){
                wm_set_snap(f, SNAP_NONE);
            } else {
                wm_minimize(f);
            }
        }
        break;
    case K_ENTER:
        if(f >= 0) wm_promote(f);
        break;
    case K_H:
        wm_adjust_ratio(-5);
        break;
    case K_L:
        wm_adjust_ratio(5);
        break;
    default:
        break;
    }
}

int wm_key(const struct yk_input_event *e){
    int code = e->keycode;
    if(code == K_LWIN || code == K_RWIN){
        int b = code == K_LWIN ? 1 : 2;
        if(e->pressed){
            C.wm.win_down |= b;
        } else {
            C.wm.win_down &= ~b;
        }
        return 1;                   /* Win nu ajunge la clienti */
    }
    if(!e->pressed){
        if(code == K_LALT || code == K_RALT){
            wm_cycle_end();
        }
        if(bit_get(code)){
            bit_set(code, 0);
            return 1;
        }
        return 0;
    }
    if(C.wm.win_down){
        win_binding(e);
        bit_set(code, 1);           /* orice tasta cu Win apasat ramane la WM, si eliberarea ei */
        return 1;
    }
    if(code == K_TAB && (e->modifiers & YK_MOD_ALT)){
        wm_cycle((e->modifiers & YK_MOD_SHIFT) ? -1 : 1);
        bit_set(code, 1);
        return 1;
    }
    if(code != K_LALT && code != K_RALT){
        wm_cycle_end();
    }
    return 0;
}

/* ---- mouse ---- */

static int is_tiled(const Window *w){
    return C.wm.mode[w->wm.ws] == WM_TILE && w->wm.snap != SNAP_MAX;
}

static int is_resizable(const Window *w){
    return C.wm.mode[w->wm.ws] == WM_FLOAT && w->wm.snap == SNAP_NONE;
}

static int edges_at(YgRect o, int x, int y){
    int inL = x < o.x + BORDER, inR = x >= o.x + o.w - BORDER;
    int inT = y < o.y + BORDER, inB = y >= o.y + o.h - BORDER;
    int e = 0;
    if(inL) e |= EDGE_L;
    if(inR) e |= EDGE_R;
    if(inT) e |= EDGE_T;
    if(inB) e |= EDGE_B;
    if((inL || inR) && y < o.y + WM_CORNER) e |= EDGE_T;
    if((inL || inR) && y >= o.y + o.h - WM_CORNER) e |= EDGE_B;
    if((inT || inB) && x < o.x + WM_CORNER) e |= EDGE_L;
    if((inT || inB) && x >= o.x + o.w - WM_CORNER) e |= EDGE_R;
    return e;
}

void wm_hit(int x, int y, WmHit *h){
    h->win = -1;
    h->zone = Z_NONE;
    h->edges = 0;
    if(y >= C.sh - WM_PANEL_H){
        h->zone = Z_PANEL;
        return;
    }
    for(int z = C.nz - 1; z >= 0; z--){
        int idx = C.zorder[z];
        const Window *w = &C.win[idx];
        if(!wm_visible(w) || w->state == WS_CLOSING){
            continue;
        }
        YgRect o = window_outer(w);
        if(!yg_rect_contains(o, x, y)){
            if(!is_resizable(w)){
                continue;
            }
            YgRect big = yg_rect(o.x - WM_GRAB, o.y - WM_GRAB, o.w + 2 * WM_GRAB, o.h + 2 * WM_GRAB);
            if(!yg_rect_contains(big, x, y)){
                continue;
            }
            h->win = idx;
            h->zone = Z_RESIZE;
            h->edges = edges_at(o, x, y);
            return;
        }
        h->win = idx;
        if(is_resizable(w)){
            int e = edges_at(o, x, y);
            if(e){
                h->zone = Z_RESIZE;
                h->edges = e;
                return;
            }
        }
        static const int btns[3] = {Z_CLOSE, Z_MAX, Z_MIN};
        for(int i = 0; i < 3; i++){
            if(yg_rect_contains(wm_btn_rect(w, btns[i]), x, y)){
                h->zone = btns[i];
                return;
            }
        }
        if(y < o.y + TITLE_H){
            h->zone = Z_TITLE;
        } else if(yg_rect_contains(window_content(w), x, y)){
            h->zone = Z_CONTENT;
        } else {
            h->zone = Z_BORDER;
        }
        return;
    }
}

int wm_busy(void){
    return C.wm.act != ACT_NONE;
}

static void set_preview(int has, YgRect r){
    if(C.wm.has_preview && (!has || r.x != C.wm.preview.x || r.y != C.wm.preview.y || r.w != C.wm.preview.w
                            || r.h != C.wm.preview.h)){
        damage(C.wm.preview);
    }
    if(has && (!C.wm.has_preview || r.x != C.wm.preview.x || r.y != C.wm.preview.y || r.w != C.wm.preview.w
               || r.h != C.wm.preview.h)){
        damage(r);
    }
    C.wm.has_preview = has;
    if(has){
        C.wm.preview = r;
    }
}

void wm_end_action(void){
    if(C.wm.act == ACT_NONE && !C.wm.has_preview){
        return;
    }
    set_preview(0, C.wm.preview);
    C.wm.act = ACT_NONE;
    C.wm.act_win = -1;
    C.wm.act_snap = 0;
}

static void start_move(int idx, const Window *w){
    C.wm.act = is_tiled(w) ? ACT_SWAP : ACT_MOVE;
    C.wm.act_win = idx;
    C.wm.act_dx = C.cx - w->x;
    C.wm.act_dy = C.cy - w->y;
    C.wm.act_moved = 0;
    /* geometria flotanta de la inceputul tragerii (la snap, restaurarea se intoarce acolo) */
    C.wm.act_ox = w->wm.snap == SNAP_NONE ? w->x : w->wm.fx;
    C.wm.act_oy = w->wm.snap == SNAP_NONE ? w->y : w->wm.fy;
    C.wm.act_w = w->wm.snap == SNAP_NONE ? w->w : w->wm.fw;
    C.wm.act_h = w->wm.snap == SNAP_NONE ? w->h : w->wm.fh;
    C.wm.act_snap = C.wm.act == ACT_SWAP ? -1 : SNAP_NONE;
}

static void start_resize(int idx, const Window *w, int edges){
    C.wm.act = ACT_RESIZE;
    C.wm.act_win = idx;
    C.wm.act_edges = edges;
    C.wm.act_cx = C.cx;
    C.wm.act_cy = C.cy;
    C.wm.act_ox = w->x;
    C.wm.act_oy = w->y;
    C.wm.act_w = w->w;
    C.wm.act_h = w->h;
    C.wm.act_notify_ms = 0;
}

int wm_press(uint8_t pressed){
    wm_cycle_end();
    WmHit h;
    wm_hit(C.cx, C.cy, &h);
    int left = (pressed & YK_BTN_LEFT) != 0;
    if(h.zone == Z_PANEL){
        if(left){
            for(int i = 0; i < WM_NWS; i++){
                if(yg_rect_contains(wm_tab_rect(i), C.cx, C.cy)){
                    wm_switch(i);
                    return -1;
                }
            }
            if(yg_rect_contains(wm_mode_rect(), C.cx, C.cy)){
                wm_set_mode(wm_mode() == WM_TILE ? WM_FLOAT : WM_TILE);
            }
        }
        return -1;
    }
    if(h.win < 0){
        focus_window(-1);
        return -1;
    }
    int idx = h.win;
    Window *w = &C.win[idx];
    raise_window(idx);
    focus_window(idx);
    switch(h.zone){
    case Z_CONTENT:
        return idx;
    case Z_CLOSE:
        if(left) wm_close(idx);
        break;
    case Z_MAX:
        if(left) wm_toggle_max(idx);
        break;
    case Z_MIN:
        if(left) wm_minimize(idx);
        break;
    case Z_TITLE:
        if(left){
            long long now = now_ms();
            if(C.wm.click_win == idx && now - C.wm.click_ms < WM_DBLCLICK_MS){
                C.wm.click_win = -1;
                wm_toggle_max(idx);
            } else {
                C.wm.click_win = idx;
                C.wm.click_ms = now;
                start_move(idx, w);
            }
        }
        break;
    case Z_RESIZE:
        if(left) start_resize(idx, w, h.edges);
        break;
    default:
        break;
    }
    return -1;
}

static int snap_at(int x, int y){
    YgRect a = wm_workarea();
    int l = x < a.x + WM_SNAP_EDGE, r = x >= a.x + a.w - WM_SNAP_EDGE;
    int t = y < a.y + WM_SNAP_EDGE, b = y >= a.y + a.h - WM_SNAP_EDGE;
    if((l || r) && y < a.y + WM_SNAP_CORNER) return l ? SNAP_TL : SNAP_TR;
    if((l || r) && y >= a.y + a.h - WM_SNAP_CORNER) return l ? SNAP_BL : SNAP_BR;
    if((t || b) && x < a.x + WM_SNAP_CORNER) return t ? SNAP_TL : SNAP_BL;
    if((t || b) && x >= a.x + a.w - WM_SNAP_CORNER) return t ? SNAP_TR : SNAP_BR;
    if(l) return SNAP_LEFT;
    if(r) return SNAP_RIGHT;
    if(t) return SNAP_MAX;
    return SNAP_NONE;
}

static int clampi(int v, int lo, int hi){
    return v < lo ? lo : v > hi ? hi : v;
}

static void do_move(void){
    int idx = C.wm.act_win;
    Window *w = &C.win[idx];
    YgRect a = wm_workarea();
    if(!C.wm.act_moved && w->wm.snap != SNAP_NONE){
        /* desprinderea dintr-un snap: revine la marimea flotanta, cursorul ramane pe aceeasi fractie a barei */
        int oldw = w->w + 2 * BORDER;
        int neww = w->wm.fw + 2 * BORDER;
        C.wm.act_dx = oldw > 0 ? (C.cx - w->x) * neww / oldw : neww / 2;
        C.wm.act_dy = clampi(C.cy - w->y, 0, TITLE_H - 1);
        w->wm.snap = SNAP_NONE;
        wm_set_frame(idx, C.cx - C.wm.act_dx, w->y, w->wm.fw, w->wm.fh);
        wm_notify_size(idx, 0);
    }
    C.wm.act_moved = 1;
    int x = C.cx - C.wm.act_dx, y = C.cy - C.wm.act_dy;
    y = clampi(y, a.y, a.y + a.h - TITLE_H);
    x = clampi(x, 40 - (w->w + 2 * BORDER), a.w - 40);
    wm_set_frame(idx, x, y, w->w, w->h);
    w->wm.fx = x;
    w->wm.fy = y;
    w->wm.fw = w->w;
    w->wm.fh = w->h;
    int s = snap_at(C.cx, C.cy);
    C.wm.act_snap = s;
    set_preview(s != SNAP_NONE, s != SNAP_NONE ? wm_snap_rect(s) : C.wm.preview);
}

static void do_resize(void){
    int idx = C.wm.act_win;
    Window *w = &C.win[idx];
    int dx = C.cx - C.wm.act_cx, dy = C.cy - C.wm.act_cy;
    int x = C.wm.act_ox, y = C.wm.act_oy, cw = C.wm.act_w, ch = C.wm.act_h;
    int e = C.wm.act_edges;
    if(e & EDGE_L){
        cw = clampi(C.wm.act_w - dx, WM_MIN_W, 4096);
        x = C.wm.act_ox + C.wm.act_w - cw;
    }
    if(e & EDGE_R){
        cw = clampi(C.wm.act_w + dx, WM_MIN_W, 4096);
    }
    if(e & EDGE_T){
        ch = clampi(C.wm.act_h - dy, WM_MIN_H, 4096);
        y = C.wm.act_oy + C.wm.act_h - ch;
    }
    if(e & EDGE_B){
        ch = clampi(C.wm.act_h + dy, WM_MIN_H, 4096);
    }
    wm_set_frame(idx, x, y, cw, ch);
    w->wm.fx = x;
    w->wm.fy = y;
    w->wm.fw = w->w;
    w->wm.fh = w->h;
    long long now = now_ms();
    if(now - C.wm.act_notify_ms >= WM_NOTIFY_MS){
        C.wm.act_notify_ms = now;
        wm_notify_size(idx, 0);
    }
}

static void do_swap(void){
    WmHit h;
    wm_hit(C.cx, C.cy, &h);
    int target = -1;
    if(h.win >= 0 && h.win != C.wm.act_win && h.zone != Z_PANEL && is_tiled(&C.win[h.win])){
        target = h.win;
    }
    C.wm.act_snap = target;
    set_preview(target >= 0, target >= 0 ? window_outer(&C.win[target]) : C.wm.preview);
}

static void hot_track(void){
    WmHit h;
    wm_hit(C.cx, C.cy, &h);
    int win = -1, btn = -1;
    if(h.zone == Z_CLOSE || h.zone == Z_MAX || h.zone == Z_MIN){
        win = h.win;
        btn = h.zone;
    }
    if(win != C.wm.hot_win || btn != C.wm.hot_btn){
        if(C.wm.hot_win >= 0 && C.win[C.wm.hot_win].used){
            damage(window_title(&C.win[C.wm.hot_win]));
        }
        C.wm.hot_win = win;
        C.wm.hot_btn = btn;
        if(win >= 0){
            damage(window_title(&C.win[win]));
        }
    }
}

void wm_motion(void){
    switch(C.wm.act){
    case ACT_MOVE:
        do_move();
        break;
    case ACT_RESIZE:
        do_resize();
        break;
    case ACT_SWAP:
        do_swap();
        break;
    default:
        hot_track();
        break;
    }
}

void wm_release(uint8_t released){
    if(!(released & YK_BTN_LEFT) || C.wm.act == ACT_NONE){
        return;
    }
    int idx = C.wm.act_win;
    int act = C.wm.act;
    int snap = C.wm.act_snap;
    int ox = C.wm.act_ox, oy = C.wm.act_oy, ow = C.wm.act_w, oh = C.wm.act_h;
    wm_end_action();
    if(idx < 0 || !C.win[idx].used){
        return;
    }
    if(act == ACT_MOVE){
        if(snap != SNAP_NONE){
            Window *w = &C.win[idx];
            w->wm.fx = ox;
            w->wm.fy = oy;
            w->wm.fw = ow;
            w->wm.fh = oh;
            wm_set_snap(idx, snap);
        }
    } else if(act == ACT_RESIZE){
        wm_notify_size(idx, 0);
    } else if(act == ACT_SWAP && snap >= 0 && C.win[snap].used){
        int t = C.win[idx].wm.tseq;
        C.win[idx].wm.tseq = C.win[snap].wm.tseq;
        C.win[snap].wm.tseq = t;
        wm_layout();
    }
    input_update_hover();
}
