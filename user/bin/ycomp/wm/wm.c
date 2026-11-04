/* Managerul de ferestre hibrid (T10.3): spatii de lucru, moduri flotant / tiling, snap, minimizare, Alt+Tab, aranjarea */
#include "comp.h"
#include <stdlib.h>
#include <string.h>

void wm_init(void){
    memset(&C.wm, 0, sizeof(C.wm));
    for(int i = 0; i < WM_NWS; i++){
        C.wm.mode[i] = WM_FLOAT;
        C.wm.ratio[i] = 55;
    }
    C.wm.hot_win = C.wm.hot_btn = -1;
    C.wm.act_win = -1;
    C.wm.click_win = -1;
}

YgRect wm_workarea(void){
    return yg_rect(0, 0, C.sw, C.sh - WM_PANEL_H);
}

int wm_visible(const Window *w){
    return w->used && w->wm.ws == C.wm.cur && !w->wm.minimized;
}

int wm_mode(void){
    return C.wm.mode[C.wm.cur];
}

int wm_ws_count(int ws){
    int n = 0;
    for(int i = 0; i < MAX_WINDOWS; i++){
        if(C.win[i].used && C.win[i].state != WS_CLOSING && C.win[i].wm.ws == ws){
            n++;
        }
    }
    return n;
}

void wm_damage_panel(void){
    damage(wm_panel_rect());
}

/* ---- geometria ---- */

static int clampi(int v, int lo, int hi){
    return v < lo ? lo : v > hi ? hi : v;
}

static int cw_of(int ow){
    int v = ow - 2 * BORDER;
    return v < 1 ? 1 : v > 4096 ? 4096 : v;
}

static int ch_of(int oh){
    int v = oh - TITLE_H - BORDER;
    return v < 1 ? 1 : v > 4096 ? 4096 : v;
}

YgRect wm_snap_rect(int snap){
    YgRect a = wm_workarea();
    int hw = a.w / 2, hh = a.h / 2;
    switch(snap){
    case SNAP_LEFT:  return yg_rect(a.x, a.y, hw, a.h);
    case SNAP_RIGHT: return yg_rect(a.x + hw, a.y, a.w - hw, a.h);
    case SNAP_MAX:   return a;
    case SNAP_TL:    return yg_rect(a.x, a.y, hw, hh);
    case SNAP_TR:    return yg_rect(a.x + hw, a.y, a.w - hw, hh);
    case SNAP_BL:    return yg_rect(a.x, a.y + hh, hw, a.h - hh);
    case SNAP_BR:    return yg_rect(a.x + hw, a.y + hh, a.w - hw, a.h - hh);
    default:         return a;
    }
}

void wm_set_frame(int idx, int x, int y, int cw, int ch){
    Window *w = &C.win[idx];
    cw = clampi(cw, 1, 4096);
    ch = clampi(ch, 1, 4096);
    if(w->x == x && w->y == y && w->w == cw && w->h == ch){
        return;
    }
    damage_window(w);
    w->x = x;
    w->y = y;
    w->w = cw;
    w->h = ch;
    damage_window(w);
}

void wm_notify_size(int idx, int force){
    Window *w = &C.win[idx];
    if(!force && w->w == w->wm.sent_w && w->h == w->wm.sent_h){
        return;
    }
    w->wm.sent_w = w->w;
    w->wm.sent_h = w->h;
    w->wm.resizes++;
    send_event(idx, YW_EV_RESIZE, w->w, w->h, 0, 0, 0);
}

/* Dreptunghi exterior -> cadru */
static void place_outer(int idx, YgRect r){
    wm_set_frame(idx, r.x, r.y, cw_of(r.w), ch_of(r.h));
    wm_notify_size(idx, 0);
}

static int by_tseq(const void *a, const void *b){
    int x = C.win[*(const int *)a].wm.tseq, y = C.win[*(const int *)b].wm.tseq;
    return x < y ? -1 : x > y;
}

/* Ferestrele spatiului curent care iau parte la aranjare, in ordinea tseq */
static int collect(int *out){
    int n = 0;
    for(int i = 0; i < MAX_WINDOWS; i++){
        const Window *w = &C.win[i];
        if(wm_visible(w) && w->state != WS_CLOSING){
            out[n++] = i;
        }
    }
    qsort(out, (size_t)n, sizeof(int), by_tseq);
    return n;
}

/* Fereastra flotanta ramane cu bara de titlu pe ecran */
static void float_clamp(int *x, int *y, int cw){
    YgRect a = wm_workarea();
    int ow = cw + 2 * BORDER;
    *x = clampi(*x, 40 - ow, a.w - 40);
    *y = clampi(*y, a.y, a.y + a.h - TITLE_H);
}

void wm_layout(void){
    int list[MAX_WINDOWS];
    int n = collect(list);
    int tiled[MAX_WINDOWS], nt = 0;
    C.wm.layouts++;
    int tile = C.wm.mode[C.wm.cur] == WM_TILE;
    for(int k = 0; k < n; k++){
        int i = list[k];
        Window *w = &C.win[i];
        if(w->wm.snap == SNAP_MAX){
            place_outer(i, wm_snap_rect(SNAP_MAX));
        } else if(tile){
            tiled[nt++] = i;
        } else if(w->wm.snap != SNAP_NONE){
            place_outer(i, wm_snap_rect(w->wm.snap));
        } else {
            int x = w->wm.fx, y = w->wm.fy;
            float_clamp(&x, &y, w->wm.fw);
            wm_set_frame(i, x, y, w->wm.fw, w->wm.fh);
            wm_notify_size(i, 0);
        }
    }
    if(nt){
        YgRect a = wm_workarea();
        int ax = a.x + WM_GAP, ay = a.y + WM_GAP, aw = a.w - 2 * WM_GAP, ah = a.h - 2 * WM_GAP;
        if(nt == 1){
            place_outer(tiled[0], yg_rect(ax, ay, aw, ah));
        } else {
            int mw = (aw - WM_GAP) * C.wm.ratio[C.wm.cur] / 100;
            place_outer(tiled[0], yg_rect(ax, ay, mw, ah));
            int sx = ax + mw + WM_GAP, sw = aw - mw - WM_GAP;
            int rows = nt - 1;
            int rh = (ah - (rows - 1) * WM_GAP) / rows;
            int y = ay;
            for(int r = 0; r < rows; r++){
                int h = r == rows - 1 ? ay + ah - y : rh;
                place_outer(tiled[1 + r], yg_rect(sx, y, sw, h));
                y += h + WM_GAP;
            }
        }
    }
    wm_damage_panel();
}

/* ---- ciclul de viata al ferestrelor ---- */

void wm_window_added(int idx){
    Window *w = &C.win[idx];
    w->wm.ws = C.wm.cur;
    w->wm.snap = SNAP_NONE;
    w->wm.fx = w->x;
    w->wm.fy = w->y;
    w->wm.fw = w->w;
    w->wm.fh = w->h;
    w->wm.tseq = ++C.wm.tseq_counter;
    w->wm.sent_w = w->w;
    w->wm.sent_h = w->h;
    wm_layout();
}

void wm_window_removed(int idx){
    if(C.wm.act != ACT_NONE && C.wm.act_win == idx){
        wm_end_action();
    }
    if(C.wm.hot_win == idx){
        C.wm.hot_win = C.wm.hot_btn = -1;
    }
    if(C.wm.click_win == idx){
        C.wm.click_win = -1;
    }
    for(int i = 0; i < C.wm.cyc_n; i++){
        if(C.wm.cyc_list[i] == idx){
            C.wm.cyc_list[i] = -1;
        }
    }
}

void wm_after_remove(void){
    wm_layout();
}

int wm_focus_best(void){
    int best = -1;
    for(int i = 0; i < MAX_WINDOWS; i++){
        const Window *w = &C.win[i];
        if(wm_visible(w) && w->state != WS_CLOSING && (best < 0 || w->wm.fseq > C.win[best].wm.fseq)){
            best = i;
        }
    }
    return best;
}

void wm_note_focus(int idx){
    if(idx >= 0 && !C.wm.cycling){
        C.win[idx].wm.fseq = ++C.wm.fseq_counter;
    }
}

void wm_client_move(int idx, int x, int y){
    Window *w = &C.win[idx];
    w->wm.fx = x;
    w->wm.fy = y;
    if(wm_visible(w) && C.wm.mode[w->wm.ws] == WM_FLOAT && w->wm.snap == SNAP_NONE){
        wm_set_frame(idx, x, y, w->w, w->h);
    }
}

void wm_close(int idx){
    send_event(idx, YW_EV_CLOSE, 0, 0, 0, 0, 0);
}

/* ---- comenzi ---- */

void wm_set_mode(int mode){
    if(C.wm.mode[C.wm.cur] == mode){
        return;
    }
    wm_end_action();
    C.wm.mode[C.wm.cur] = mode;
    wm_layout();
    input_update_hover();
}

void wm_switch(int ws){
    if(ws < 0 || ws >= WM_NWS || ws == C.wm.cur){
        return;
    }
    wm_end_action();
    wm_cycle_end();
    if(C.grab >= 0){
        send_event(C.grab, YW_EV_MOUSE, 0, 0, 0, 0, YW_MOUSE_RELEASE);
        C.grab = -1;
    }
    if(C.hover >= 0){
        send_event(C.hover, YW_EV_LEAVE, 0, 0, 0, 0, 0);
        C.hover = -1;
    }
    C.wm.hot_win = C.wm.hot_btn = -1;
    C.wm.cur = ws;
    C.wm.switches++;
    damage(yg_rect(0, 0, C.sw, C.sh));
    wm_layout();
    int best = wm_focus_best();
    if(C.focus >= 0 && !wm_visible(&C.win[C.focus])){
        send_event(C.focus, YW_EV_FOCUS, 0, 0, 0, 0, 0);
        C.focus = -1;
    }
    focus_window(best);
    input_update_hover();
}

void wm_move_to_ws(int idx, int ws){
    if(idx < 0 || ws < 0 || ws >= WM_NWS || C.win[idx].wm.ws == ws){
        return;
    }
    Window *w = &C.win[idx];
    wm_end_action();
    damage_window(w);
    w->wm.ws = ws;
    w->wm.tseq = ++C.wm.tseq_counter;
    if(C.focus == idx){
        send_event(idx, YW_EV_FOCUS, 0, 0, 0, 0, 0);
        C.focus = -1;
        focus_window(wm_focus_best());
    }
    if(C.hover == idx){
        send_event(idx, YW_EV_LEAVE, 0, 0, 0, 0, 0);
        C.hover = -1;
    }
    if(C.grab == idx){
        C.grab = -1;
    }
    wm_layout();
    input_update_hover();
}

void wm_set_snap(int idx, int snap){
    Window *w = &C.win[idx];
    wm_end_action();
    w->wm.snap = snap;
    wm_layout();
    raise_window(idx);
    input_update_hover();
}

void wm_toggle_max(int idx){
    wm_set_snap(idx, C.win[idx].wm.snap == SNAP_MAX ? SNAP_NONE : SNAP_MAX);
}

void wm_minimize(int idx){
    Window *w = &C.win[idx];
    wm_end_action();
    damage_window(w);
    w->wm.minimized = 1;
    if(C.hover == idx){
        send_event(idx, YW_EV_LEAVE, 0, 0, 0, 0, 0);
        C.hover = -1;
    }
    if(C.grab == idx){
        C.grab = -1;
    }
    if(C.focus == idx){
        send_event(idx, YW_EV_FOCUS, 0, 0, 0, 0, 0);
        C.focus = -1;
        focus_window(wm_focus_best());
    }
    wm_layout();
    input_update_hover();
}

void wm_promote(int idx){
    if(C.wm.mode[C.wm.cur] != WM_TILE){
        return;
    }
    int list[MAX_WINDOWS];
    int n = collect(list);
    int low = n ? C.win[list[0]].wm.tseq : 0;
    C.win[idx].wm.tseq = low - 1;
    wm_layout();
}

void wm_adjust_ratio(int delta){
    int *r = &C.wm.ratio[C.wm.cur];
    int v = clampi(*r + delta, WM_RATIO_MIN, WM_RATIO_MAX);
    if(v != *r){
        *r = v;
        wm_layout();
    }
}

/* ---- Alt+Tab ---- */

static int by_mru(const void *a, const void *b){
    uint32_t x = C.win[*(const int *)a].wm.fseq, y = C.win[*(const int *)b].wm.fseq;
    return x > y ? -1 : x < y;
}

void wm_cycle_end(void){
    if(!C.wm.cycling){
        return;
    }
    C.wm.cycling = 0;
    wm_note_focus(C.focus);
}

void wm_cycle(int dir){
    if(!C.wm.cycling){
        int n = 0;
        for(int i = 0; i < MAX_WINDOWS && n < 32; i++){
            const Window *w = &C.win[i];
            if(w->used && w->state != WS_CLOSING && w->wm.ws == C.wm.cur){
                C.wm.cyc_list[n++] = i;
            }
        }
        qsort(C.wm.cyc_list, (size_t)n, sizeof(int), by_mru);
        C.wm.cyc_n = n;
        C.wm.cyc_pos = 0;
        C.wm.cycling = 1;
    }
    int n = C.wm.cyc_n;
    if(n < 2){
        return;
    }
    for(int tries = 0; tries < n; tries++){
        C.wm.cyc_pos = (C.wm.cyc_pos + dir + n) % n;
        int idx = C.wm.cyc_list[C.wm.cyc_pos];
        if(idx < 0 || !C.win[idx].used || C.win[idx].state == WS_CLOSING){
            continue;
        }
        Window *w = &C.win[idx];
        if(w->wm.minimized){
            w->wm.minimized = 0;
            wm_layout();
        }
        raise_window(idx);
        focus_window(idx);
        input_update_hover();
        return;
    }
}

/* ---- introspectie (YW_Q_WM) ---- */

void wm_query(int32_t id, int32_t v[6]){
    if(id == 0){
        int tiles = 0;
        for(int i = 0; i < WM_NWS; i++){
            tiles |= (C.wm.mode[i] == WM_TILE) << i;
        }
        int vis = 0;
        for(int i = 0; i < MAX_WINDOWS; i++){
            vis += wm_visible(&C.win[i]) && C.win[i].state != WS_CLOSING;
        }
        v[0] = C.wm.cur;
        v[1] = tiles;
        v[2] = vis;
        v[3] = (int32_t)C.wm.layouts;
        v[4] = C.focus >= 0 ? (int32_t)C.win[C.focus].id : 0;
        v[5] = (int32_t)C.wm.switches;
        return;
    }
    for(int i = 0; i < MAX_WINDOWS; i++){
        const Window *w = &C.win[i];
        if(w->used && w->id == (uint32_t)id){
            int z = -1;
            for(int k = 0; k < C.nz; k++){
                if(C.zorder[k] == i){
                    z = k;
                }
            }
            v[0] = w->wm.ws;
            v[1] = (w->wm.minimized ? 1 : 0) | (w->wm.snap << 4) | ((wm_visible(w) ? 1 : 0) << 8);
            v[2] = (int32_t)w->wm.resizes;
            v[3] = w->bw;
            v[4] = w->bh;
            v[5] = z;
            return;
        }
    }
    v[0] = -1;
}
