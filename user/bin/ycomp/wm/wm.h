#ifndef WM_H
#define WM_H
/* Managerul de ferestre hibrid al ycomp (T10.3). Fiecare spatiu de lucru (4) e fie flotant (ferestre suprapuse, mutate si
 * redimensionate cu mouse-ul, snap la margini), fie tiling master-stack; ferestrele au decoratii (bara de titlu cu butoane
 * minimizare / maximizare / inchidere) desenate in stilul temei. Taste (Win = Super):
 *   Win+T tiling, Win+F flotant (spatiul curent)    Win+1..4 spatiu de lucru, Win+Shift+1..4 muta fereastra acolo
 *   Alt+Tab / Alt+Shift+Tab  fereastra urmatoare / precedenta (cea mai recenta prima, readuce si ferestrele minimizate)
 *   Win+Q inchide, Win+M minimizeaza, Win+X maximizeaza / restaureaza, Win+Stanga / Dreapta snap pe jumatate,
 *   Win+Sus maximizeaza, Win+Jos restaureaza (sau minimizeaza), Win+Enter fereastra devine master, Win+H / Win+L
 *   micsoreaza / mareste master-ul
 * Mouse: bara de titlu trage (dublu clic = maximizare; langa marginea ecranului snap, in tiling schimba locul cu
 * fereastra de sub cursor), marginile si colturile redimensioneaza (doar flotant), panoul de jos schimba spatiul / modul. */
#include "comp.h"

struct yk_input_event;

typedef struct {
    int win;                /* index in C.win[] sau -1 */
    int zone;               /* WmZone */
    int edges;              /* Z_RESIZE: EDGE_* */
} WmHit;

/* wm.c */
void wm_init(void);
YgRect wm_workarea(void);
int wm_visible(const Window *w);            /* pe spatiul curent si nemimizata */
int wm_mode(void);                           /* modul spatiului curent */
void wm_window_added(int idx);
void wm_window_removed(int idx);             /* inainte de stergere */
void wm_after_remove(void);
void wm_layout(void);                        /* aseaza ferestrele spatiului curent dupa mod / snap */
void wm_set_mode(int mode);
void wm_switch(int ws);
void wm_move_to_ws(int idx, int ws);
void wm_set_snap(int idx, int snap);         /* SNAP_NONE restaureaza */
void wm_toggle_max(int idx);
void wm_minimize(int idx);
void wm_promote(int idx);
void wm_adjust_ratio(int delta);
void wm_cycle(int dir);
void wm_cycle_end(void);
void wm_close(int idx);                      /* cere clientului sa se inchida */
void wm_client_move(int idx, int x, int y);
int wm_focus_best(void);                     /* fereastra vizibila cu focusul cel mai recent sau -1 */
void wm_note_focus(int idx);
void wm_notify_size(int idx, int force);     /* YW_EV_RESIZE daca dimensiunea s-a schimbat */
void wm_set_frame(int idx, int x, int y, int cw, int ch);   /* coltul ferestrei + continutul */
YgRect wm_snap_rect(int snap);               /* dreptunghiul exterior pentru un snap */
int wm_ws_count(int ws);                     /* ferestre pe spatiul ws */
void wm_damage_panel(void);
void wm_query(int32_t id, int32_t v[6]);

/* decor.c */
YgRect wm_btn_rect(const Window *w, int zone);   /* Z_CLOSE / Z_MAX / Z_MIN */
void wm_draw_decor(const Window *w, int focused, YgRect clip);
void wm_draw_panel(YgRect clip);
void wm_draw_preview(YgRect clip);
YgRect wm_panel_rect(void);
YgRect wm_tab_rect(int ws);
YgRect wm_mode_rect(void);

/* wmin.c */
int wm_key(const struct yk_input_event *e);  /* 1 = luata de WM, nu se da clientului */
void wm_hit(int x, int y, WmHit *h);
int wm_press(uint8_t pressed);               /* indexul ferestrei daca e clic in continut (clientul il primeste), altfel -1 */
void wm_motion(void);
void wm_release(uint8_t released);
int wm_busy(void);                           /* mutare / redimensionare in curs */
void wm_end_action(void);

#endif
