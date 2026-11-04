/* wmtest (T10.3): verificari pentru managerul de ferestre al ycomp. Rulat de test=wm, iese cu 0 daca totul trece.
 * Porneste ycomp, deschide 5 ferestre (o singura conexiune) si comanda totul prin evenimente de input injectate
 * (INPUTCTL_INJECT), ca de la tastatura / mouse: decoratii, mutare si redimensionare cu mouse-ul (clientul primeste
 * YW_EV_RESIZE si isi schimba bufferul), maximizare, snap la margini, minimizare + Alt+Tab, mod tiling master-stack (geometrie
 * exacta, nicio suprapunere), comutare Win+T / Win+F de 10 ori cu 5 ferestre (cat dureaza), swap prin tragere, Win+Enter /
 * Win+H / Win+L, spatii de lucru (taste si panou), Win+Q, tastele Win / Alt+Tab care nu ajung la clienti.
 * Daca exista /etc/wmhold, in puncte cheie asteapta 3 s dupa linia "wmtest: shot <nume>" (captura de ecran din QEMU). */
#include "ywin.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/yinput.h>

#define SOCK  "/run/wmtest-ycomp.sock"
#define NW    6
#define WW    320
#define WH    200
/* Aceleasi valori ca in user/bin/ycomp/wm/wmtypes.h */
#define GAP         4
#define PANEL_H     24
#define MIN_W       120
#define BORDER      YW_BORDER
#define TITLE_H     YW_TITLE_H

/* Codurile logice ale tastelor (kernel/include/keycodes.h) */
#define K_1 2
#define K_2 3
#define K_TAB 15
#define K_Q 16
#define K_T 20
#define K_A 30
#define K_ENTER 28
#define K_F 33
#define K_H 35
#define K_L 38
#define K_M 50
#define K_LALT 55
#define K_LEFT 75
#define K_LWIN 82

static int checks, fails;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)){ \
        fails++; \
        printf("wmtest: FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

static long long ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void msleep(int n){
    struct timespec ts = {n / 1000, (long)(n % 1000) * 1000000L};
    while(nanosleep(&ts, &ts) < 0 && errno == EINTR){
    }
}

typedef struct {
    YwWindow *w;
    uint32_t id;
    YgColor col;
    int keys, closes, resizes, mouse;
} Win;

static YwConn *ctl;
static Win W[NW];
static int sw, sh, wa_h;
static int cur_x, cur_y, btns;
static int hold;

static Win *by_id(uint32_t id){
    for(int i = 0; i < NW; i++){
        if(W[i].w && W[i].id == id){
            return &W[i];
        }
    }
    return NULL;
}

static void paint(Win *w){
    YgSurface *s = yw_window_surface(w->w);
    yg_clear(s, w->col);
    yg_fill_rect(s, 4, 4, 40, 20, YG_RGB(255, 255, 255));
    yw_window_damage_all(w->w);
}

static void handle(const YwEvent *ev){
    Win *w = by_id(ev->win);
    if(!w){
        return;
    }
    if(ev->type == YW_EV_KEY && ev->pressed){
        w->keys++;
    } else if(ev->type == YW_EV_CLOSE){
        w->closes++;
    } else if(ev->type == YW_EV_RESIZE){
        w->resizes++;
        paint(w);                       /* libywin a facut deja bufferul nou */
    } else if(ev->type == YW_EV_MOUSE && ev->kind == YW_MOUSE_PRESS){
        w->mouse++;
    }
}

static void pump(int t){
    long long end = ms() + t;
    for(;;){
        long long left = end - ms();
        YwEvent ev;
        int r = yw_next_event(ctl, &ev, left > 0 ? (int)left : 0);
        if(r <= 0){
            return;
        }
        handle(&ev);
    }
}

#define WAIT(cond, t) ({ \
    long long _e = ms() + (t); \
    int _ok = 0; \
    for(;;){ \
        if(cond){ _ok = 1; break; } \
        if(ms() > _e){ break; } \
        pump(15); \
    } \
    _ok; })

static int32_t gq(int i){
    int32_t v[6] = {0};
    yw_query(ctl, YW_Q_WM, 0, 0, v);
    return v[i];
}

static void wq(int n, int32_t v[6]){
    memset(v, 0, 6 * sizeof(int32_t));
    yw_query(ctl, YW_Q_WM, (int)W[n].id, 0, v);
}

static int snap_of(int n){
    int32_t v[6];
    wq(n, v);
    return (v[1] >> 4) & 15;
}

static int minimized(int n){
    int32_t v[6];
    wq(n, v);
    return v[1] & 1;
}

static int visible(int n){
    int32_t v[6];
    wq(n, v);
    return (v[1] >> 8) & 1;
}

/* Continutul ferestrei n pe ecran: x, y, w, h */
static void crect(int n, int r[4]){
    int32_t v[6] = {0};
    yw_query(ctl, YW_Q_WIN, (int)W[n].id, 0, v);
    for(int i = 0; i < 4; i++){
        r[i] = v[i];
    }
}

static int rect_is(int n, int x, int y, int w, int h){
    int r[4];
    crect(n, r);
    return r[0] == x && r[1] == y && r[2] == w && r[3] == h;
}

/* Bufferul atasat are dimensiunea cadrului (clientul a raspuns la YW_EV_RESIZE) */
static int synced(int n){
    int32_t v[6];
    int r[4];
    wq(n, v);
    crect(n, r);
    return v[3] == r[2] && v[4] == r[3];
}

static int all_synced(int count){
    for(int i = 0; i < count; i++){
        if(!synced(i)){
            return 0;
        }
    }
    return 1;
}

static uint32_t pixel(int x, int y){
    int32_t v[6] = {0};
    yw_query(ctl, YW_Q_PIXEL, x, y, v);
    return (uint32_t)v[0];
}

static void cursor_query(int *x, int *y){
    int32_t v[6] = {0};
    yw_query(ctl, YW_Q_STATS2, 0, 0, v);
    *x = v[0];
    *y = v[1];
}

/* ---- intrare injectata ---- */

static void inject_mouse(int dx, int dy){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_MOUSE;
    e.dx = (int16_t)dx;
    e.dy = (int16_t)dy;
    e.buttons = (uint8_t)btns;
    inputctl(INPUTCTL_INJECT, &e);
}

static int clampi(int v, int lo, int hi){
    return v < lo ? lo : v > hi ? hi : v;
}

static void mouse_to(int x, int y){
    x = clampi(x, 0, sw - 1);
    y = clampi(y, 0, sh - 1);
    while(cur_x != x || cur_y != y){
        int dx = clampi(x - cur_x, -100, 100), dy = clampi(y - cur_y, -100, 100);
        inject_mouse(dx, dy);
        cur_x += dx;
        cur_y += dy;
    }
}

static int cursor_synced(void){
    int x, y;
    cursor_query(&x, &y);
    return x == cur_x && y == cur_y;
}

static void sync_cursor(void){
    int ok = WAIT(cursor_synced(), 3000);
    CHECK(ok, "cursorul a ajuns la %d,%d", cur_x, cur_y);
}

static void press(void){
    btns = YK_BTN_LEFT;
    inject_mouse(0, 0);
}

static void release(void){
    btns = 0;
    inject_mouse(0, 0);
}

static void click(int x, int y){
    mouse_to(x, y);
    sync_cursor();
    press();
    release();
    pump(60);
}

static void drag(int x0, int y0, int x1, int y1){
    mouse_to(x0, y0);
    sync_cursor();
    press();
    pump(30);
    for(int i = 1; i <= 4; i++){
        mouse_to(x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4);
        pump(30);
    }
    sync_cursor();
    release();
    pump(60);
}

static void key_event(int code, int ascii, int pressed, int mods){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_KEY;
    e.keycode = (uint16_t)code;
    e.ascii = (uint8_t)ascii;
    e.pressed = (uint8_t)pressed;
    e.modifiers = (uint8_t)mods;
    inputctl(INPUTCTL_INJECT, &e);
}

static void win_key(int code, int mods){
    key_event(K_LWIN, 0, 1, mods);
    key_event(code, 0, 1, mods);
    key_event(code, 0, 0, mods);
    key_event(K_LWIN, 0, 0, 0);
    pump(40);
}

static void alt_tab(int shift){
    int mods = YK_MOD_ALT | (shift ? YK_MOD_SHIFT : 0);
    key_event(K_TAB, 0, 1, mods);
    key_event(K_TAB, 0, 0, mods);
    pump(60);
}

static void shot(const char *name){
    if(hold){
        pump(300);
        printf("wmtest: shot %s\n", name);
        pump(3000);
    }
}

/* Pozitia tintei in coordonate de ecran */
static void outer_of(int n, int *x, int *y, int *w, int *h){
    int r[4];
    crect(n, r);
    *x = r[0] - BORDER;
    *y = r[1] - TITLE_H;
    *w = r[2] + 2 * BORDER;
    *h = r[3] + TITLE_H + BORDER;
}

/* Geometria asteptata a ferestrei i (din n) in master-stack, continut */
static void tile_expect(int n, int ratio, int i, int out[4]){
    int ax = GAP, ay = GAP, aw = sw - 2 * GAP, ah = wa_h - 2 * GAP;
    int ox, oy, ow, oh;
    if(n == 1){
        ox = ax; oy = ay; ow = aw; oh = ah;
    } else if(i == 0){
        ox = ax; oy = ay; ow = (aw - GAP) * ratio / 100; oh = ah;
    } else {
        int mw = (aw - GAP) * ratio / 100;
        int rows = n - 1;
        int rh = (ah - (rows - 1) * GAP) / rows;
        int r = i - 1;
        ox = ax + mw + GAP;
        ow = aw - mw - GAP;
        oy = ay + r * (rh + GAP);
        oh = r == rows - 1 ? ay + ah - oy : rh;
    }
    out[0] = ox + BORDER;
    out[1] = oy + TITLE_H;
    out[2] = ow - 2 * BORDER;
    out[3] = oh - TITLE_H - BORDER;
}

static int tile_ok(int n, int ratio, const int order[]){
    for(int i = 0; i < n; i++){
        int e[4];
        tile_expect(n, ratio, i, e);
        if(!rect_is(order[i], e[0], e[1], e[2], e[3])){
            return 0;
        }
    }
    return 1;
}

static int overlaps(int a, int b){
    int x[4], y[4];
    crect(a, x);
    crect(b, y);
    return x[0] < y[0] + y[2] && y[0] < x[0] + x[2] && x[1] < y[1] + y[3] && y[1] < x[1] + x[3];
}

static int create(int i, int w, int h, const char *title){
    W[i].w = yw_window_new(ctl, w, h, title);
    if(!W[i].w){
        return -1;
    }
    W[i].id = yw_window_id(W[i].w);
    W[i].col = YG_RGB(40 + 30 * i, 200 - 25 * i, 90 + 20 * i);
    paint(&W[i]);
    return 0;
}

static void test_wm(void){
    pid_t comp = fork();
    if(comp == 0){
        char *const av[] = {"ycomp", "-s", SOCK, "-q", NULL};
        execv("/bin/ycomp", av);
        _exit(127);
    }
    for(int i = 0; i < 200 && !ctl; i++){
        ctl = yw_connect(SOCK);
        if(!ctl){
            msleep(50);
        }
    }
    CHECK(ctl != NULL, "conectare la ycomp: errno %d", errno);
    if(!ctl){
        kill(comp, SIGKILL);
        waitpid(comp, NULL, 0);
        return;
    }
    sw = yw_screen_width(ctl);
    sh = yw_screen_height(ctl);
    wa_h = sh - PANEL_H;
    CHECK(sw >= 640 && sh >= 480, "ecran %dx%d", sw, sh);
    cursor_query(&cur_x, &cur_y);

    /* --- 1. cinci ferestre flotante, decoratii --- */
    static const char *const titles[5] = {"Unu", "Doi", "Trei", "Patru", "Cinci"};
    for(int i = 0; i < 5; i++){
        CHECK(create(i, WW, WH, titles[i]) == 0, "fereastra %d: errno %d", i, errno);
        yw_window_move(W[i].w, 30 + i * 60, 30 + i * 40);
    }
    pump(500);                                      /* animatiile de deschidere se termina */
    CHECK(gq(0) == 0 && gq(1) == 0 && gq(2) == 5, "pornire: spatiu %d, moduri %x, vizibile %d", gq(0), gq(1), gq(2));
    CHECK(gq(4) == (int)W[4].id, "ultima fereastra are focus");
    int r4[4], r3[4];
    crect(4, r4);
    crect(3, r3);
    CHECK(r4[0] == 30 + 240 + BORDER && r4[1] == 30 + 160 + TITLE_H, "pozitie flotanta: %d,%d", r4[0], r4[1]);
    const YgTheme *th = &yg_theme_dark;
    uint32_t accent = th->accent & 0xFFFFFF, alt = th->surface_alt & 0xFFFFFF;
    CHECK(pixel(r4[0] + 60, r4[1] - TITLE_H / 2) == accent, "bara ferestrei active in culoarea accent: %06x", pixel(r4[0] + 60, r4[1] - TITLE_H / 2));
    CHECK(pixel(r3[0] + 60, r3[1] - TITLE_H / 2) == alt, "bara ferestrei inactive: %06x", pixel(r3[0] + 60, r3[1] - TITLE_H / 2));
    int ox4, oy4, ow4, oh4;
    outer_of(4, &ox4, &oy4, &ow4, &oh4);
    uint32_t bar = pixel(r4[0] + 60, r4[1] - TITLE_H / 2);
    int bx = ox4 + ow4 - 6 - 16;                    /* inchidere */
    int by = oy4 + (TITLE_H - 16) / 2;
    CHECK(pixel(bx + 3, by + 8) != bar, "butonul de inchidere desenat");
    CHECK(pixel(bx - 20 + 3, by + 8) != bar, "butonul de maximizare desenat");
    CHECK(pixel(bx - 40 + 3, by + 8) != bar, "butonul de minimizare desenat");
    CHECK(pixel(bx - 60 - 20, by + 8) == bar, "bara goala intre titlu si butoane");
    CHECK(pixel(r4[0] - BORDER, r4[1] + 50) == accent, "chenarul ferestrei active");
    CHECK(pixel(sw / 2, sh - PANEL_H / 2) != 0 && pixel(5, sh - 1) == (th->surface_alt & 0xFFFFFF), "panoul de jos desenat: %06x", pixel(5, sh - 1));
    shot("float");

    /* --- 2. mutare cu bara de titlu (fereastra 2) --- */
    int ox, oy, ow, oh;
    outer_of(4, &ox, &oy, &ow, &oh);
    int c2[4];
    crect(4, c2);
    drag(ox + 40, oy + TITLE_H / 2, ox + 140, oy + TITLE_H / 2 + 50);
    int ok = WAIT(rect_is(4, c2[0] + 100, c2[1] + 50, c2[2], c2[3]), 3000);
    CHECK(ok, "mutare cu 100,50");
    CHECK(gq(4) == (int)W[4].id, "focus pe fereastra mutata");
    int32_t v[6];
    wq(4, v);
    CHECK(v[5] == 4, "fereastra mutata e deasupra: z %d", v[5]);
    CHECK(W[4].mouse == 0, "clientul nu primeste mouse-ul tras pe bara de titlu: %d", W[4].mouse);
    int moved4[4];
    crect(4, moved4);

    /* --- 3. redimensionare din coltul dreapta-jos si din marginea stanga --- */
    outer_of(4, &ox, &oy, &ow, &oh);
    int rs0 = W[4].resizes;
    drag(ox + ow + 2, oy + oh + 2, ox + ow + 62, oy + oh + 42);
    ok = WAIT(rect_is(4, moved4[0], moved4[1], moved4[2] + 60, moved4[3] + 40) && synced(4), 4000);
    CHECK(ok, "redimensionare +60,+40 (si bufer atasat)");
    CHECK(W[4].resizes > rs0, "clientul a primit YW_EV_RESIZE");
    crect(4, c2);
    CHECK(pixel(c2[0] + c2[2] - 5, c2[1] + c2[3] - 5) == (W[4].col & 0xFFFFFF), "zona noua redesenata de client: %06x", pixel(c2[0] + c2[2] - 5, c2[1] + c2[3] - 5));
    outer_of(4, &ox, &oy, &ow, &oh);
    drag(ox - 2, oy + oh / 2, sw - 1, oy + oh / 2);
    ok = WAIT(rect_is(4, ox + ow - 2 * BORDER - MIN_W + BORDER, c2[1], MIN_W, c2[3]) && synced(4), 4000);
    {
        int r[4];
        crect(4, r);
        CHECK(ok, "latimea minima %d: acum %d la %d", MIN_W, r[2], r[0]);
    }
    shot("resized");

    /* --- 4. maximizare cu butonul, apoi restaurare --- */
    crect(1, c2);
    outer_of(1, &ox, &oy, &ow, &oh);
    click(ox + ow - 42 + 8 - 0, oy + TITLE_H / 2);
    ok = WAIT(snap_of(1) == 3 && rect_is(1, BORDER, TITLE_H, sw - 2 * BORDER, wa_h - TITLE_H - BORDER) && synced(1), 3000);
    CHECK(ok, "maximizare: snap %d", snap_of(1));
    outer_of(1, &ox, &oy, &ow, &oh);
    click(ox + ow - 42 + 8, oy + TITLE_H / 2);
    ok = WAIT(snap_of(1) == 0 && rect_is(1, c2[0], c2[1], c2[2], c2[3]) && synced(1), 3000);
    CHECK(ok, "restaurare dupa maximizare");

    /* --- 5. snap la margine (trage in stanga) si la colt --- */
    crect(3, c2);
    outer_of(3, &ox, &oy, &ow, &oh);
    drag(ox + ow - 110, oy + TITLE_H / 2, 2, 300);
    ok = WAIT(snap_of(3) == 1 && rect_is(3, BORDER, TITLE_H, sw / 2 - 2 * BORDER, wa_h - TITLE_H - BORDER) && synced(3), 3000);
    CHECK(ok, "snap stanga: snap %d", snap_of(3));
    win_key(K_LEFT, 0);                             /* Win+Stanga o dezactiveaza */
    ok = WAIT(snap_of(3) == 0 && rect_is(3, c2[0], c2[1], c2[2], c2[3]) && synced(3), 3000);
    CHECK(ok, "Win+Stanga restaureaza geometria de dinainte de snap");
    outer_of(3, &ox, &oy, &ow, &oh);
    pump(500);                                      /* altfel doua apasari pe titlu sunt dublu clic (maximizare) */
    drag(ox + ow - 110, oy + TITLE_H / 2, sw - 1, 0);
    ok = WAIT(snap_of(3) == 5, 3000);                /* colt dreapta-sus */
    CHECK(ok, "snap in colt: %d", snap_of(3));
    {
        int hw = sw / 2, hh = wa_h / 2;
        CHECK(rect_is(3, hw + BORDER, TITLE_H, sw - hw - 2 * BORDER, hh - TITLE_H - BORDER), "geometria colt dreapta-sus");
    }
    win_key(K_LEFT, 0);                             /* nu e snap stanga: devine snap stanga */
    WAIT(snap_of(3) == 1, 2000);
    win_key(K_LEFT, 0);
    ok = WAIT(snap_of(3) == 0 && rect_is(3, c2[0], c2[1], c2[2], c2[3]), 3000);
    CHECK(ok, "restaurare dupa snap");

    /* dublu clic pe bara de titlu: maximizeaza, apoi restaureaza */
    pump(500);
    outer_of(3, &ox, &oy, &ow, &oh);
    click(ox + ow - 110, oy + TITLE_H / 2);
    click(ox + ow - 110, oy + TITLE_H / 2);
    ok = WAIT(snap_of(3) == 3 && synced(3), 3000);
    CHECK(ok, "dublu clic pe titlu maximizeaza: snap %d", snap_of(3));
    pump(500);
    outer_of(3, &ox, &oy, &ow, &oh);
    click(ox + ow - 110, oy + TITLE_H / 2);
    click(ox + ow - 110, oy + TITLE_H / 2);
    ok = WAIT(snap_of(3) == 0 && rect_is(3, c2[0], c2[1], c2[2], c2[3]) && synced(3), 3000);
    CHECK(ok, "al doilea dublu clic restaureaza");

    /* --- 6. minimizare si Alt+Tab --- */
    CHECK(gq(4) == (int)W[3].id, "focus pe fereastra 3");
    win_key(K_M, 0);
    ok = WAIT(minimized(3) == 1 && gq(2) == 4, 2000);
    CHECK(ok, "Win+M minimizeaza: vizibile %d", gq(2));
    CHECK(gq(4) == (int)W[1].id, "focusul trece la fereastra cea mai recenta: %d", gq(4));
    alt_tab(0);
    ok = WAIT(minimized(3) == 0 && gq(4) == (int)W[3].id && gq(2) == 5, 2000);
    CHECK(ok, "Alt+Tab readuce fereastra minimizata");
    alt_tab(0);
    ok = WAIT(gq(4) == (int)W[4].id, 2000);
    CHECK(ok, "al doilea Alt+Tab: urmatoarea in ordinea recenta (%d)", gq(4));
    alt_tab(1);
    ok = WAIT(gq(4) == (int)W[3].id, 2000);
    CHECK(ok, "Alt+Shift+Tab: inapoi");
    key_event(K_LALT, 0, 0, 0);                     /* Alt eliberat: ciclul se incheie */
    pump(60);
    int keys_before = 0;
    for(int i = 0; i < NW; i++){
        keys_before += W[i].keys;
    }
    CHECK(keys_before == 0, "nicio tasta Win / Alt+Tab nu a ajuns la clienti: %d", keys_before);
    key_event(K_A, 'a', 1, 0);
    key_event(K_A, 0, 0, 0);
    ok = WAIT(W[3].keys == 1, 2000);
    CHECK(ok, "o tasta obisnuita ajunge la fereastra activa (%d)", W[3].keys);

    /* --- 7. tiling master-stack --- */
    int fl[5][4];
    for(int i = 0; i < 5; i++){
        crect(i, fl[i]);
    }
    uint32_t layouts0 = (uint32_t)gq(3);
    long long t0 = ms();
    win_key(K_T, 0);
    ok = WAIT(gq(1) == 1, 2000);
    CHECK(ok, "Win+T: modul tiling pe spatiul 0 (masca %x)", gq(1));
    static const int order[5] = {0, 1, 2, 3, 4};
    ok = WAIT(tile_ok(5, 55, order) && all_synced(5), 4000);
    long long dt = ms() - t0;
    CHECK(ok, "geometrie master-stack cu 5 ferestre");
    printf("wmtest: Win+T cu 5 ferestre: %lld ms pana la geometrie + buffere\n", dt);
    CHECK((uint32_t)gq(3) - layouts0 <= 3, "o singura aranjare pe comutare: %u", (uint32_t)gq(3) - layouts0);
    int clean = 1;
    for(int a = 0; a < 5; a++){
        for(int b = a + 1; b < 5; b++){
            if(overlaps(a, b)){
                clean = 0;
            }
        }
    }
    CHECK(clean, "ferestrele tiled nu se suprapun");
    shot("tile");

    /* comutari repetate */
    long long worst = 0, total = 0;
    int good = 1;
    for(int i = 0; i < 10; i++){
        int to_float = (i & 1) == 0;
        long long a = ms();
        win_key(to_float ? K_F : K_T, 0);
        int want_mask = to_float ? 0 : 1;
        int done = WAIT(gq(1) == want_mask && all_synced(5), 4000);
        if(done && to_float){
            for(int k = 0; k < 5; k++){
                done = done && rect_is(k, fl[k][0], fl[k][1], fl[k][2], fl[k][3]);
            }
        } else if(done){
            done = tile_ok(5, 55, order);
        }
        long long d = ms() - a;
        total += d;
        if(d > worst){
            worst = d;
        }
        good = good && done;
    }
    CHECK(good, "10 comutari Win+F / Win+T: geometria exacta de fiecare data");
    CHECK(worst < 1500, "cea mai lenta comutare: %lld ms", worst);
    printf("wmtest: 10 comutari float/tile cu 5 ferestre: medie %lld ms, maxim %lld ms\n", total / 10, worst);
    CHECK(gq(1) == 1, "dupa 10 comutari (ultima Win+T) suntem in tiling");

    /* --- 8. operatii in tiling --- */
    win_key(K_L, 0);
    ok = WAIT(tile_ok(5, 60, order) && all_synced(5), 3000);
    CHECK(ok, "Win+L mareste master-ul (60%%)");
    win_key(K_H, 0);
    win_key(K_H, 0);
    ok = WAIT(tile_ok(5, 50, order) && all_synced(5), 3000);
    CHECK(ok, "Win+H x2 micsoreaza master-ul (50%%)");
    win_key(K_L, 0);
    WAIT(tile_ok(5, 55, order), 3000);
    /* swap: trage fereastra 2 (stack) peste fereastra 4 (stack) */
    int o2[4], o4[4];
    crect(2, o2);
    crect(4, o4);
    drag(o2[0] + 60, o2[1] - TITLE_H / 2, o4[0] + 60, o4[1] + 40);
    static const int swapped[5] = {0, 1, 4, 3, 2};
    ok = WAIT(tile_ok(5, 55, swapped) && all_synced(5), 4000);
    CHECK(ok, "tragere peste alta fereastra: schimb de locuri");
    CHECK(W[2].mouse == 0, "tragerea in tiling nu ajunge la client");
    /* Win+Enter: fereastra activa (4? cea trasa) devine master */
    int focused = gq(4) == (int)W[2].id ? 2 : -1;
    CHECK(focused == 2, "fereastra trasa are focus");
    win_key(K_ENTER, 0);
    static const int promoted[5] = {2, 0, 1, 4, 3};
    ok = WAIT(tile_ok(5, 55, promoted) && all_synced(5), 4000);
    CHECK(ok, "Win+Enter: fereastra activa devine master");

    /* --- 9. spatii de lucru --- */
    win_key(K_2, 0);
    ok = WAIT(gq(0) == 1 && gq(2) == 0, 2000);
    CHECK(ok, "Win+2: spatiul 1 gol (spatiu %d, vizibile %d)", gq(0), gq(2));
    CHECK(visible(0) == 0 && visible(3) == 0, "ferestrele celuilalt spatiu sunt ascunse");
    CHECK(pixel(sw / 2, wa_h / 2) != (W[0].col & 0xFFFFFF) && pixel(sw / 2, wa_h / 2) != (W[2].col & 0xFFFFFF), "si nu se mai deseneaza");
    CHECK(create(5, 240, 160, "Sase") == 0, "fereastra pe spatiul 1");
    pump(300);
    {
        int32_t q[6];
        wq(5, q);
        CHECK(q[0] == 1 && gq(2) == 1 && gq(4) == (int)W[5].id, "fereastra noua pe spatiul 1 cu focus (ws %d)", q[0]);
    }
    key_event(K_LWIN, 0, 1, 0);
    key_event(K_1, 0, 1, YK_MOD_SHIFT);
    key_event(K_1, 0, 0, YK_MOD_SHIFT);
    key_event(K_LWIN, 0, 0, 0);
    ok = WAIT(gq(2) == 0 && visible(5) == 0, 2000);
    CHECK(ok, "Win+Shift+1 muta fereastra pe spatiul 0");
    win_key(K_1, 0);
    ok = WAIT(gq(0) == 0 && gq(2) == 6, 2000);
    CHECK(ok, "Win+1: inapoi, 6 ferestre (vizibile %d)", gq(2));
    static const int six[6] = {2, 0, 1, 4, 3, 5};
    ok = WAIT(tile_ok(6, 55, six) && all_synced(6), 4000);
    CHECK(ok, "6 ferestre in tiling dupa mutare");
    click(6 + 2 * 32 + 14, sh - PANEL_H / 2);
    ok = WAIT(gq(0) == 2, 2000);
    CHECK(ok, "clic pe tab-ul 3 din panou (spatiu %d)", gq(0));
    click(6 + 0 * 32 + 14, sh - PANEL_H / 2);
    ok = WAIT(gq(0) == 0, 2000);
    CHECK(ok, "clic pe tab-ul 1");
    click(6 + 4 * 32 + 6 + 32, sh - PANEL_H / 2);
    ok = WAIT(gq(1) == 0, 2000);
    CHECK(ok, "clic pe eticheta de mod trece in flotant (masca %x)", gq(1));
    shot("float2");
    win_key(K_T, 0);
    WAIT(gq(1) == 1, 2000);

    /* --- 10. Win+Q, apoi inchidere --- */
    int fid = gq(4);
    Win *fw = by_id((uint32_t)fid);
    int c0 = fw ? fw->closes : 0;
    win_key(K_Q, 0);
    ok = WAIT(fw && fw->closes == c0 + 1, 2000);
    CHECK(ok, "Win+Q cere inchiderea ferestrei active");
    int total_keys = 0;
    for(int i = 0; i < NW; i++){
        total_keys += W[i].keys;
    }
    CHECK(total_keys == 1, "taste primite de clienti in tot testul: %d (doar 'a')", total_keys);

    for(int i = 0; i < NW; i++){
        if(W[i].w){
            yw_window_destroy(W[i].w);
            W[i].w = NULL;
        }
    }
    ok = WAIT(gq(2) == 0, 3000);
    CHECK(ok, "nicio fereastra dupa inchidere (%d)", gq(2));
    yw_disconnect(ctl);
    ctl = NULL;
    kill(comp, SIGTERM);
    int stc = -1;
    waitpid(comp, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "ycomp iese curat la SIGTERM: %d", stc);
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    hold = access("/etc/wmhold", F_OK) == 0;
    test_wm();
    printf("wmtest: %d verificari, %d esecuri\n", checks, fails);
    if(fails){
        return 1;
    }
    printf("[OK] wmtest\n");
    return 0;
}
