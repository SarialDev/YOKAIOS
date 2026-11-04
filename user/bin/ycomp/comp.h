#ifndef YCOMP_H
#define YCOMP_H
/* ycomp (T10.2): compozitorul de ferestre al YokaiOS. Starea comuna a modulelor main.c / render.c / input.c */
#include <stdint.h>
#include "ygfx.h"
#include "ywin_proto.h"
#include "wm/wmtypes.h"

#define MAX_CLIENTS   16
#define MAX_WINDOWS   32
#define MAX_DIRTY     16
#define TITLE_H       YW_TITLE_H
#define BORDER        YW_BORDER
#define CLOSE_SIZE    YW_CLOSE_SIZE

enum WinState { WS_OPENING, WS_NORMAL, WS_CLOSING };

typedef struct Window {
    int used;
    uint32_t id;
    int client;             /* indexul clientului, -1 dupa deconectare (fereastra se stinge) */
    int x, y;               /* coltul ferestrei (cu decoratiuni) pe ecran */
    int w, h;               /* continutul (cat da loc cadrul; il hotaraste managerul de ferestre) */
    int bw, bh;             /* dimensiunea buferului atasat (poate ramane in urma lui w x h pana raspunde clientul) */
    int role;               /* YW_ROLE_*: panou / popup n-au decoratii: x, y, w, h sunt chiar continutul (T10.4) */
    int tsent;              /* panoul a primit YW_EV_TASK pentru fereastra asta */
    int tflags;             /* ... cu aceste YW_TF_* */
    uint32_t thash;         /* ... si cu titlul cu aceasta suma */
    char title[32];
    uint32_t *pixels;       /* buferul shm mapat (NULL pana la YW_ATTACH) */
    void *map;              /* inceputul maparii (pixels = map + offset) */
    size_t map_len;
    int stride;
    enum WinState state;
    long long anim_start;   /* ms */
    uint8_t opacity;        /* 255 = opac */
    int pending_frame;      /* are damage compus in cadrul urmator: trimite YW_FRAME dupa */
    uint32_t damage_msgs, frames_sent;
    WmWin wm;               /* T10.3 */
} Window;

typedef struct {
    int fd;                 /* -1 = liber */
    uint8_t in[1024];
    int inlen;
    uint8_t out[16384];     /* mesaje netrimise inca (socket-ul clientului e neblocant) */
    int outlen;
    uint32_t dropped;       /* evenimente pierdute: clientul nu citea */
} Client;

typedef struct {
    YgFb fb;
    YgSurface *back;        /* back buffer: aici se compune, apoi se copiaza pe ecran (flip) */
    YgSurface *wall;        /* fundalul, desenat o data */
    YgSurface *cursor;      /* sageata (cu alfa) */
    YgFont *font;
    const YgTheme *theme;
    int sw, sh;

    Window win[MAX_WINDOWS];
    int zorder[MAX_WINDOWS];    /* indici in win[], de jos in sus */
    int nz;
    uint32_t next_id;
    int focus;              /* index in win[] sau -1 */
    int hover;              /* fereastra sub cursor (continut) sau -1 */
    int grab;               /* fereastra care a primit apasarea (primeste mouse-ul pana la eliberare) sau -1 */
    int drag;               /* fereastra mutata de bara de titlu sau -1 */
    int drag_dx, drag_dy;
    Client cl[MAX_CLIENTS];

    int cx, cy;             /* cursor */
    uint8_t buttons;

    YgRect dirty[MAX_DIRTY];
    int ndirty;

    /* ritm */
    int fps;                /* 60, sau 30 cand RT e incarcat */
    int anim;               /* animatiile pornite */
    int fps_max;
    long long epoch_us;     /* cadrele sunt la epoch + k * perioada */
    long long next_frame_us;
    long long last_k;       /* indicele ultimului cadru compus (cel mult unul pe perioada) */
    int frame_scheduled;
    long long rt_checked_ms;
    int rt_calm;            /* verificari consecutive fara incarcare */
    uint64_t rt_misses;
    uint32_t rt_load_ppm;
    uint32_t degraded_count;

    /* statistici */
    uint32_t frames, wakeups;
    uint64_t pixels;
    int quiet;
    WmState wm;             /* T10.3 */
    int panel_h;            /* inaltimea panoului ypanel (0 = fara; atunci apare panoul simplu al WM) */
    int last_nfocus;        /* ultima fereastra normala cu focus (panoul o arata activa si cat un meniu are focusul) */
    uint32_t task_msgs, start_msgs;
} Comp;

extern Comp C;

long long now_ms(void);
long long now_us(void);

/* render.c */
int render_init(const char *theme);
void render_fini(void);
void damage(YgRect r);
void damage_window(const Window *w);       /* tot cadrul ferestrei */
void damage_cursor(void);
YgRect window_outer(const Window *w);
YgRect window_content(const Window *w);
YgRect window_title(const Window *w);
YgRect window_close_box(const Window *w);
void compose_and_flip(void);                /* compune zonele murdare in back buffer si le copiaza pe ecran */
uint32_t back_pixel(int x, int y);

/* input.c */
void input_event(const void *ev);           /* struct yk_input_event */
int window_at(int x, int y);                /* fereastra de sus care contine punctul (cadrul intreg) sau -1 */
void focus_window(int idx);
void raise_window(int idx);
void keep_special_on_top(void);

/* main.c */
void send_event(int widx, uint32_t type, int v0, int v1, int v2, int v3, int v4);
void window_remove(int idx);                /* fara animatie */
void request_frame(void);                   /* exista ceva de compus: se programeaza urmatorul cadru */
void input_update_hover(void);

#include "wm/wm.h"

#endif
