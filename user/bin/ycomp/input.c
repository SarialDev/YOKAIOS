/* ycomp: cursorul software si rutarea intrarii (tastatura la fereastra cu focus, mouse-ul la cea de sub cursor) */
#include "comp.h"
#include <sys/yinput.h>
#include <signal.h>
#include <string.h>

int window_at(int x, int y){
    for(int z = C.nz - 1; z >= 0; z--){
        const Window *w = &C.win[C.zorder[z]];
        if(wm_visible(w) && w->state != WS_CLOSING && yg_rect_contains(window_outer(w), x, y)){
            return C.zorder[z];
        }
    }
    return -1;
}

/* Panoul si popup-urile raman deasupra ferestrelor normale (partitie stabila a z-order-ului) */
void keep_special_on_top(void){
    int tmp[MAX_WINDOWS], n = 0;
    for(int z = 0; z < C.nz; z++){
        if(C.win[C.zorder[z]].role == YW_ROLE_NORMAL){
            tmp[n++] = C.zorder[z];
        }
    }
    for(int z = 0; z < C.nz; z++){
        if(C.win[C.zorder[z]].role != YW_ROLE_NORMAL){
            tmp[n++] = C.zorder[z];
        }
    }
    memcpy(C.zorder, tmp, (size_t)n * sizeof(int));
}

void raise_window(int idx){
    int pos = -1;
    for(int z = 0; z < C.nz; z++){
        if(C.zorder[z] == idx){
            pos = z;
        }
    }
    if(pos < 0 || pos == C.nz - 1){
        return;
    }
    for(int z = pos; z + 1 < C.nz; z++){
        C.zorder[z] = C.zorder[z + 1];
    }
    C.zorder[C.nz - 1] = idx;
    keep_special_on_top();
    damage_window(&C.win[idx]);
}

void focus_window(int idx){
    if(idx == C.focus){
        return;
    }
    if(C.focus >= 0){
        send_event(C.focus, YW_EV_FOCUS, 0, 0, 0, 0, 0);
        damage(window_title(&C.win[C.focus]));
    }
    C.focus = idx;
    if(idx >= 0 && C.win[idx].role == YW_ROLE_NORMAL){
        C.last_nfocus = idx;
    }
    wm_note_focus(idx);
    if(idx >= 0){
        send_event(idx, YW_EV_FOCUS, 1, 0, 0, 0, 0);
        damage(window_title(&C.win[idx]));
    }
}

static int in_content(int idx, int x, int y){
    return idx >= 0 && yg_rect_contains(window_content(&C.win[idx]), x, y);
}

static void local(int idx, int *lx, int *ly){
    YgRect c = window_content(&C.win[idx]);
    *lx = C.cx - c.x;
    *ly = C.cy - c.y;
}

void input_update_hover(void){
    int top = window_at(C.cx, C.cy);
    int h = in_content(top, C.cx, C.cy) ? top : -1;
    if(h == C.hover){
        return;
    }
    if(C.hover >= 0){
        send_event(C.hover, YW_EV_LEAVE, 0, 0, 0, 0, 0);
    }
    C.hover = h;
    if(h >= 0){
        int lx, ly;
        local(h, &lx, &ly);
        send_event(h, YW_EV_ENTER, lx, ly, 0, 0, 0);
    }
}

static void mouse(const struct yk_input_event *e){
    int ox = C.cx, oy = C.cy;
    C.cx += e->dx;
    C.cy += e->dy;
    if(C.cx < 0) C.cx = 0;
    if(C.cy < 0) C.cy = 0;
    if(C.cx > C.sw - 1) C.cx = C.sw - 1;
    if(C.cy > C.sh - 1) C.cy = C.sh - 1;
    int moved = C.cx != ox || C.cy != oy;
    if(moved){
        damage(yg_rect(ox, oy, 12, 19));
        damage_cursor();
    }
    uint8_t old = C.buttons;
    C.buttons = e->buttons;
    uint8_t pressed = (uint8_t)(C.buttons & ~old);
    uint8_t released = (uint8_t)(old & ~C.buttons);

    if(moved){
        wm_motion();
    }
    if(pressed){
        int idx = wm_press(pressed);
        if(idx >= 0){
            C.grab = idx;
            int lx, ly;
            local(idx, &lx, &ly);
            send_event(idx, YW_EV_MOUSE, lx, ly, C.buttons, 0, YW_MOUSE_PRESS);
        }
    }
    int target = C.grab >= 0 ? C.grab : C.hover;
    if(moved && target >= 0 && !wm_busy()){
        int lx, ly;
        local(target, &lx, &ly);
        send_event(target, YW_EV_MOUSE, lx, ly, C.buttons, 0, YW_MOUSE_MOVE);
    }
    if(e->dz && C.hover >= 0){
        int lx, ly;
        local(C.hover, &lx, &ly);
        send_event(C.hover, YW_EV_MOUSE, lx, ly, C.buttons, e->dz, YW_MOUSE_WHEEL);
    }
    if(released){
        if(C.grab >= 0){
            int lx, ly;
            local(C.grab, &lx, &ly);
            send_event(C.grab, YW_EV_MOUSE, lx, ly, C.buttons, 0, YW_MOUSE_RELEASE);
        }
        if(!C.buttons){
            C.grab = -1;
        }
        wm_release(released);
    }
    input_update_hover();
}

static void key(const struct yk_input_event *e){
    /* Ctrl+Alt+Backspace: iesire din compozitor (ecranul revine consolei) */
    if(e->pressed && e->keycode == YK_KEY_BACKSPACE && (e->modifiers & YK_MOD_CTRL) && (e->modifiers & YK_MOD_ALT)){
        raise(SIGTERM);
        return;
    }
    if(wm_key(e)){
        return;
    }
    if(C.focus >= 0){
        send_event(C.focus, YW_EV_KEY, e->keycode, e->ascii, e->pressed, e->modifiers, 0);
    }
}

void input_event(const void *p){
    const struct yk_input_event *e = p;
    if(e->type == YK_EV_MOUSE){
        mouse(e);
    } else if(e->type == YK_EV_KEY){
        key(e);
    }
}
