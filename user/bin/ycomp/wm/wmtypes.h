#ifndef WM_TYPES_H
#define WM_TYPES_H
/* Managerul de ferestre hibrid (T10.3): tipurile din starea compozitorului (Window.wm si Comp.wm) */
#include "ygfx.h"

#define WM_NWS          4       /* spatii de lucru */
#define WM_GAP          4       /* spatiu intre ferestrele tiled si fata de margini */
#define WM_PANEL_H      24      /* panoul de jos (spatii de lucru, mod, fereastra activa) */
#define WM_SNAP_EDGE    6       /* cat de aproape de margine (pixeli) incepe snap-ul */
#define WM_SNAP_CORNER  64      /* lungimea zonei de colt pe margine */
#define WM_MIN_W        120     /* continut minim la redimensionare */
#define WM_MIN_H        60
#define WM_GRAB         5       /* zona de redimensionare in afara chenarului */
#define WM_CORNER       12      /* zona de colt pentru redimensionare pe doua axe */
#define WM_BTN_GAP      4
#define WM_DBLCLICK_MS  400
#define WM_NOTIFY_MS    60      /* ritmul YW_EV_RESIZE cat se trage de margine */
#define WM_RATIO_MIN    20
#define WM_RATIO_MAX    80

enum WmMode { WM_FLOAT = 0, WM_TILE = 1 };

enum WmSnap { SNAP_NONE = 0, SNAP_LEFT, SNAP_RIGHT, SNAP_MAX, SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR };

/* Unde a nimerit cursorul */
enum WmZone { Z_NONE = 0, Z_CONTENT, Z_TITLE, Z_CLOSE, Z_MAX, Z_MIN, Z_BORDER, Z_RESIZE, Z_PANEL };

enum { EDGE_L = 1, EDGE_R = 2, EDGE_T = 4, EDGE_B = 8 };

enum WmAct { ACT_NONE = 0, ACT_MOVE, ACT_RESIZE, ACT_SWAP };

typedef struct WmWin {
    int ws;                 /* spatiul de lucru */
    int minimized;
    int snap;               /* WmSnap */
    int fx, fy, fw, fh;     /* geometria flotanta: coltul ferestrei, continutul */
    int tseq;               /* ordinea in tiling (cel mai mic = master) */
    uint32_t fseq;          /* cand a primit focus ultima oara (Alt+Tab: cel mai recent primul) */
    int sent_w, sent_h;     /* ultima dimensiune cerita clientului (YW_EV_RESIZE) */
    uint32_t resizes;       /* cate YW_EV_RESIZE s-au trimis */
} WmWin;

typedef struct WmState {
    int cur;                        /* spatiul de lucru curent */
    int mode[WM_NWS];
    int ratio[WM_NWS];              /* latimea master-ului, procente */
    uint32_t fseq_counter;
    int tseq_counter;
    uint32_t layouts, switches;

    int win_down;                   /* bit 0 = Win stanga, bit 1 = Win dreapta */
    uint8_t consumed[16];           /* tastele a caror apasare a luat-o WM (si eliberarea) */
    int cycling;                    /* Alt+Tab in curs */
    int cyc_list[32];
    int cyc_n, cyc_pos;

    int act;                        /* WmAct */
    int act_win;
    int act_dx, act_dy;             /* mutare: cursor - coltul ferestrei */
    int act_edges;
    int act_cx, act_cy;             /* redimensionare: cursorul la start */
    int act_ox, act_oy, act_w, act_h;   /* si geometria ferestrei la start */
    int act_moved;
    long long act_notify_ms;
    int act_snap;                   /* snap-ul ales in timpul mutarii */
    int has_preview;
    YgRect preview;
    int hot_win, hot_btn;           /* butonul de sub cursor */
    long long click_ms;
    int click_win;
} WmState;

#endif
