/* paneltest (T10.4): verificari pentru panoul ypanel (taskbar + meniul Start) si pentru rolurile PANEL / POPUP ale ycomp.
 * Rulat de test=panel; iese cu 0 daca totul trece (cu /etc/panelhold asteapta 3 s la "paneltest: shot <nume>" pentru capturi). Porneste ycomp (socket-ul implicit, ca sa-l gaseasca si aplicatiile
 * pornite de panou) si /bin/ypanel, apoi comanda totul prin input injectat (INPUTCTL_INJECT), ca de la tastatura / mouse:
 *   - zona de lucru: o fereastra maximizata se opreste deasupra panoului (24 px al WM, apoi 40 px ypanel si inapoi)
 *   - bara desenata: fundal, logo-ul Yokai in centru, aplicatia ancorata (Terminal) in stanga
 *   - clic pe aplicatia ancorata porneste /bin/yterm, care apare in dreapta, activ (butonul in culoarea accent)
 *   - clic pe butonul ferestrei: minimizeaza, apoi readuce cu focus; clic dreapta inchide
 *   - tasta Win (singura) deschide meniul Start, inca o data il inchide; Esc il inchide; clic in alta parte il inchide
 *   - cautare: "bil" lasa o singura aplicatie; Enter o porneste; din meniu se porneste si Terminalul
 *   - Win+litera ramane combinatie a WM (nu deschide meniul); yterm primeste taste si executa `exit`
 *   - ypanel iese curat la SIGTERM si zona de lucru revine */
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

/* Aceleasi valori ca in user/bin/ypanel/{panel.h,bar.c,menu.c} si user/bin/ycomp */
#define PANEL_H     40
#define WM_PANEL_H  24
#define BORDER      YW_BORDER
#define TITLE_H     YW_TITLE_H
#define START_W     46
#define PIN_X       8
#define PIN_W       38
#define MENU_LIST_Y 54
#define MENU_ROW_H  36
#define MENU_PAD    8

#define K_A     30
#define K_Q     16
#define K_UP    73
#define K_LWIN  82
#define K_ENTER 28
#define K_ESC   1
#define K_BS    14

static int checks, fails;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)){ \
        fails++; \
        printf("paneltest: FAIL %s:%d: ", __FILE__, __LINE__); \
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

static YwConn *ctl;
static YwWindow *tw;            /* fereastra de proba */
static int sw, sh;
static int cur_x, cur_y, btns;
static const YgTheme *th = &yg_theme_dark;
static int hold;

static void pump(int t);

/* Cu /etc/panelhold, in puncte cheie asteapta 3 s dupa linia "paneltest: shot <nume>" (captura de ecran din QEMU) */
static void shot(const char *name){
    if(hold){
        pump(300);
        printf("paneltest: shot %s\n", name);
        pump(3000);
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
        if(ev.type == YW_EV_RESIZE && tw){
            yg_clear(yw_window_surface(tw), YG_RGB(60, 90, 200));
            yw_window_damage_all(tw);
        }
    }
}

#define WAIT(cond, t) ({ \
    long long _e = ms() + (t); \
    int _ok = 0; \
    for(;;){ \
        if(cond){ _ok = 1; break; } \
        if(ms() > _e){ break; } \
        pump(20); \
    } \
    _ok; })

static void q(int kind, int a, int b, int32_t v[6]){
    memset(v, 0, 6 * sizeof(int32_t));
    yw_query(ctl, kind, a, b, v);
}

static int panel_q(int i){
    int32_t v[6];
    q(YW_Q_PANEL, 0, 0, v);
    return v[i];
}

static int wm_q(int i){
    int32_t v[6];
    q(YW_Q_WM, 0, 0, v);
    return v[i];
}

static int win_minimized(uint32_t id){
    int32_t v[6];
    q(YW_Q_WM, (int)id, 0, v);
    return v[0] >= 0 && (v[1] & 1);
}

static int win_exists(uint32_t id){
    int32_t v[6];
    q(YW_Q_WM, (int)id, 0, v);
    return v[0] >= 0;
}

static uint32_t pixel(int x, int y){
    int32_t v[6];
    q(YW_Q_PIXEL, x, y, v);
    return (uint32_t)v[0];
}

static void content(uint32_t id, int r[4]){
    int32_t v[6];
    q(YW_Q_WIN, (int)id, 0, v);
    for(int i = 0; i < 4; i++){
        r[i] = v[i];
    }
}

/* ---- intrare injectata ---- */

static int clampi(int v, int lo, int hi){
    return v < lo ? lo : v > hi ? hi : v;
}

static void inject_mouse(int dx, int dy){
    struct yk_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = YK_EV_MOUSE;
    e.dx = (int16_t)dx;
    e.dy = (int16_t)dy;
    e.buttons = (uint8_t)btns;
    inputctl(INPUTCTL_INJECT, &e);
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
    int32_t v[6];
    q(YW_Q_STATS2, 0, 0, v);
    return v[0] == cur_x && v[1] == cur_y;
}

static void click_btn(int x, int y, int button){
    mouse_to(x, y);
    int ok = WAIT(cursor_synced(), 3000);
    CHECK(ok, "cursorul a ajuns la %d,%d", x, y);
    btns = button;
    inject_mouse(0, 0);
    btns = 0;
    inject_mouse(0, 0);
    pump(80);
}

static void click(int x, int y){
    click_btn(x, y, YK_BTN_LEFT);
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

static void tap(int code, int ascii, int mods){
    key_event(code, ascii, 1, mods);
    key_event(code, ascii, 0, mods);
}

static void type_str(const char *s){
    for(; *s; s++){
        tap(K_A, *s, 0);
        pump(15);
    }
    pump(60);
}

static void win_tap(void){
    key_event(K_LWIN, 0, 1, 0);
    key_event(K_LWIN, 0, 0, 0);
    pump(60);
}

static void win_combo(int code, int mods){
    key_event(K_LWIN, 0, 1, mods);
    key_event(code, 0, 1, mods);
    key_event(code, 0, 0, mods);
    key_event(K_LWIN, 0, 0, 0);
    pump(60);
}

/* Geometria barei (oglinda a lui bar.c) */
static int start_cx(void){ return sw / 2; }
static int bar_cy(void){ return sh - PANEL_H / 2; }
static int task_x(int i, int total){
    int x0 = sw / 2 + START_W / 2 + 14;
    int x1 = sw - 8 - 58 - 4 - 76 - 10;
    int bw = (x1 - x0 - (total - 1) * 4) / total;
    if(bw > 172) bw = 172;
    if(bw < 28) bw = 28;
    return x0 + i * (bw + 4);
}

/* Fereastra meniului: x, y, w, h (continut) sau 0 */
static int menu_rect(int r[4]){
    int id = panel_q(4);
    if(!id){
        return 0;
    }
    content((uint32_t)id, r);
    return r[2] > 0;
}

static int row_icon_pixel(const int r[4], int row){
    return (int)pixel(r[0] + MENU_PAD + 6 + 3, r[1] + MENU_LIST_Y + row * MENU_ROW_H + (MENU_ROW_H - 2 - 24) / 2 + 12);
}

static pid_t spawn(const char *path, char *const av[]){
    pid_t p = fork();
    if(p == 0){
        execv(path, av);
        _exit(127);
    }
    return p;
}

static void test_panel(void){
    pid_t comp = spawn("/bin/ycomp", (char *const[]){"ycomp", "-q", NULL});
    for(int i = 0; i < 200 && !ctl; i++){
        ctl = yw_connect(NULL);
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
    CHECK(sw >= 640 && sh >= 480, "ecran %dx%d", sw, sh);
    int32_t v[6];
    q(YW_Q_PANEL, 0, 0, v);
    CHECK(v[0] == 0 && v[1] == 0 && v[4] == 0 && v[5] == 0, "fara panou: h %d id %d popup %d ferestre %d", v[0], v[1], v[4], v[5]);
    q(YW_Q_THEME, 0, 0, v);
    CHECK(v[0] == 0, "tema implicita e cea intunecata");
    {
        int32_t s[6];
        q(YW_Q_STATS2, 0, 0, s);
        cur_x = s[0];
        cur_y = s[1];
    }

    /* --- 1. zona de lucru: fereastra maximizata se opreste deasupra panoului --- */
    tw = yw_window_new(ctl, 320, 200, "Proba");
    CHECK(tw != NULL, "fereastra de proba: errno %d", errno);
    if(!tw){
        return;
    }
    uint32_t tid = yw_window_id(tw);
    yg_clear(yw_window_surface(tw), YG_RGB(60, 90, 200));
    yw_window_damage_all(tw);
    pump(400);
    win_combo(K_UP, 0);                             /* Win+Sus = maximizare */
    int r[4];
    int ok = WAIT((content(tid, r), r[1] + r[3] + BORDER == sh - WM_PANEL_H), 3000);
    CHECK(ok, "maximizata fara ypanel: baza la %d, asteptat %d", r[1] + r[3] + BORDER, sh - WM_PANEL_H);

    /* --- 2. ypanel porneste: rezerva 40 px, deseneaza bara --- */
    pid_t panel = spawn("/bin/ypanel", (char *const[]){"ypanel", NULL});
    ok = WAIT(panel_q(0) == PANEL_H, 8000);
    CHECK(ok, "ypanel a cerut %d px (a fost %d)", PANEL_H, panel_q(0));
    ok = WAIT((content(tid, r), r[1] + r[3] + BORDER == sh - PANEL_H), 3000);
    CHECK(ok, "maximizata cu ypanel: baza la %d, asteptat %d", r[1] + r[3] + BORDER, sh - PANEL_H);
    CHECK(panel_q(1) != 0, "id-ul ferestrei panou");
    ok = WAIT(panel_q(2) >= 2, 3000);
    CHECK(ok, "ypanel a primit starea (YW_EV_TASK trimise: %d)", panel_q(2));
    uint32_t bar_bg = th->surface_alt & 0xFFFFFF;
    ok = WAIT(pixel(5, sh - 2) == bar_bg, 3000);
    CHECK(ok, "fundalul barei: %06x", pixel(5, sh - 2));
    CHECK(pixel(5, sh - PANEL_H) == (th->border & 0xFFFFFF), "linia de sus a barei: %06x", pixel(5, sh - PANEL_H));
    int logo = 0;
    for(int y = sh - PANEL_H + 4; y < sh - 4; y++){
        for(int x = start_cx() - 14; x < start_cx() + 14; x++){
            logo += pixel(x, y) == (th->logo & 0xFFFFFF);
        }
    }
    CHECK(logo > 40, "logo Yokai in centrul barei: %d pixeli", logo);
    int pin = 0;
    for(int x = PIN_X + 4; x < PIN_X + PIN_W - 4; x++){
        pin += pixel(x, bar_cy()) == (th->accent & 0xFFFFFF);
    }
    CHECK(pin > 6, "aplicatia ancorata (Terminal) in stanga: %d pixeli accent", pin);
    /* fereastra de proba apare in dreapta, activa */
    ok = WAIT(pixel(task_x(0, 1) + 4, bar_cy()) == (th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "butonul ferestrei de proba, activ: %06x", pixel(task_x(0, 1) + 4, bar_cy()));
    yw_window_destroy(tw);
    tw = NULL;
    ok = WAIT(!win_exists(tid) && panel_q(5) == 0, 3000);
    CHECK(ok, "fereastra de proba inchisa");
    ok = WAIT(pixel(task_x(0, 1) + 4, bar_cy()) == bar_bg, 3000);
    CHECK(ok, "butonul ei a disparut din bara: %06x", pixel(task_x(0, 1) + 4, bar_cy()));

    /* --- 3. clic pe aplicatia ancorata: Terminalul apare in dreapta --- */
    click(PIN_X + PIN_W / 2, bar_cy());
    ok = WAIT(panel_q(5) == 1, 8000);
    CHECK(ok, "Terminalul pornit din bara (ferestre %d)", panel_q(5));
    ok = WAIT(pixel(task_x(0, 1) + 4, bar_cy()) == (th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "butonul Terminalului in dreapta, activ: %06x", pixel(task_x(0, 1) + 4, bar_cy()));
    shot("bar");
    int term1 = wm_q(4);
    CHECK(term1 != 0, "Terminalul are focus");

    /* --- 4. clic pe buton: minimizeaza, apoi readuce --- */
    click(task_x(0, 1) + 40, bar_cy());
    ok = WAIT(win_minimized((uint32_t)term1), 3000);
    CHECK(ok, "clic pe butonul ferestrei active o minimizeaza");
    ok = WAIT(pixel(task_x(0, 1) + 4, bar_cy()) != (th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "butonul minimizat nu mai e activ");
    click(task_x(0, 1) + 40, bar_cy());
    ok = WAIT(!win_minimized((uint32_t)term1) && wm_q(4) == term1, 3000);
    CHECK(ok, "clic pe butonul minimizat o readuce cu focus");

    /* --- 5. Start cu tasta Win --- */
    int st0 = panel_q(3);
    win_tap();
    ok = WAIT(panel_q(3) == st0 + 1 && panel_q(4) != 0, 4000);
    CHECK(ok, "Win singura deschide meniul (YW_EV_START %d, popup %d)", panel_q(3), panel_q(4));
    int m[4] = {0};
    CHECK(menu_rect(m), "meniul are fereastra");
    shot("menu");
    CHECK(m[1] + m[3] <= sh - PANEL_H && m[0] >= 0 && m[0] + m[2] <= sw && m[3] >= 150, "meniul deasupra barei: %d,%d %dx%d", m[0], m[1], m[2], m[3]);
    CHECK(wm_q(2) == 1, "meniul nu e fereastra normala (ferestre vizibile %d)", wm_q(2));
    ok = WAIT(row_icon_pixel(m, 0) == (int)(th->accent_text & 0xFFFFFF), 3000);
    CHECK(ok, "primul rand selectat: %06x", row_icon_pixel(m, 0));
    CHECK(row_icon_pixel(m, 1) == (int)(th->accent & 0xFFFFFF), "al doilea rand exista, neselectat: %06x", row_icon_pixel(m, 1));
    tap(K_UP + 5, 0, 0);                            /* Jos (78) */
    pump(80);
    ok = WAIT(row_icon_pixel(m, 1) == (int)(th->accent_text & 0xFFFFFF) && row_icon_pixel(m, 0) == (int)(th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "sageata jos muta selectia");
    win_tap();
    ok = WAIT(panel_q(4) == 0, 3000);
    CHECK(ok, "inca o data Win inchide meniul");

    /* --- 6. cautare: "bil" -> Bile --- */
    win_tap();
    WAIT(panel_q(4) != 0, 3000);
    CHECK(menu_rect(m), "meniul s-a redeschis");
    type_str("bil");
    ok = WAIT(row_icon_pixel(m, 1) == (int)(th->surface & 0xFFFFFF), 3000);
    CHECK(ok, "dupa \"bil\" a ramas un singur rand: %06x", row_icon_pixel(m, 1));
    CHECK(row_icon_pixel(m, 0) == (int)(th->accent_text & 0xFFFFFF), "...si e selectat");
    shot("search");
    tap(K_BS, 0, 0);
    tap(K_BS, 0, 0);
    tap(K_BS, 0, 0);
    pump(100);
    ok = WAIT(row_icon_pixel(m, 1) == (int)(th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "Backspace x3 readuce lista");
    type_str("bil");
    tap(K_ENTER, 0, 0);
    ok = WAIT(panel_q(5) == 2, 8000);
    CHECK(ok, "Enter porneste Bile (ferestre %d)", panel_q(5));
    ok = WAIT(panel_q(4) == 0 || !win_exists((uint32_t)panel_q(4)), 3000);
    CHECK(ok, "meniul se inchide dupa pornire");
    ok = WAIT(pixel(task_x(1, 2) + 4, bar_cy()) == (th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "al doilea buton (Bile) in bara, activ: %06x", pixel(task_x(1, 2) + 4, bar_cy()));
    int bile = wm_q(4);
    shot("two");

    /* --- 7. Esc, clic pe Start, clic in alta parte --- */
    win_tap();
    WAIT(panel_q(4) != 0, 3000);
    tap(K_ESC, 0, 0);
    ok = WAIT(panel_q(4) == 0, 3000);
    CHECK(ok, "Esc inchide meniul");
    click(start_cx(), bar_cy());
    ok = WAIT(panel_q(4) != 0, 3000);
    CHECK(ok, "clic pe logo deschide meniul");
    click(start_cx(), bar_cy());
    ok = WAIT(panel_q(4) == 0, 3000);
    CHECK(ok, "clic pe logo il inchide");
    win_tap();
    WAIT(panel_q(4) != 0, 3000);
    click(sw - 6, 6);                               /* desktop gol */
    ok = WAIT(panel_q(4) == 0, 3000);
    CHECK(ok, "clic pe desktop inchide meniul");

    /* --- 8. Win + alta tasta ramane combinatie a WM, nu deschide meniul --- */
    st0 = panel_q(3);
    win_combo(K_A, 0);
    pump(100);
    CHECK(panel_q(3) == st0 && panel_q(4) == 0, "Win+tasta nu deschide meniul");

    /* --- 9. Terminalul din meniul Start; yterm primeste taste --- */
    win_tap();
    WAIT(panel_q(4) != 0, 3000);
    type_str("term");
    tap(K_ENTER, 0, 0);
    ok = WAIT(panel_q(5) == 3, 8000);
    CHECK(ok, "Terminalul pornit din Start (ferestre %d)", panel_q(5));
    ok = WAIT(pixel(task_x(2, 3) + 4, bar_cy()) == (th->accent & 0xFFFFFF), 3000);
    CHECK(ok, "al treilea buton, activ: %06x", pixel(task_x(2, 3) + 4, bar_cy()));
    msleep(300);
    type_str("echo salut");
    tap(K_ENTER, 0, 0);
    msleep(500);
    pump(100);
    type_str("exit");
    tap(K_ENTER, 0, 0);
    ok = WAIT(panel_q(5) == 2, 8000);
    CHECK(ok, "`exit` in yterm il inchide (ferestre %d)", panel_q(5));

    /* --- 10. clic dreapta pe buton inchide fereastra; Win+Q --- */
    click_btn(task_x(1, 2) + 40, bar_cy(), YK_BTN_RIGHT);
    ok = WAIT(panel_q(5) == 1 && !win_exists((uint32_t)bile), 8000);
    CHECK(ok, "clic dreapta pe butonul Bile o inchide (ferestre %d)", panel_q(5));
    for(int i = 0; i < 6 && panel_q(5) > 0; i++){
        win_combo(K_Q, 0);
        WAIT(panel_q(5) == 0, 800);
    }
    ok = WAIT(panel_q(5) == 0, 5000);
    CHECK(ok, "toate ferestrele inchise (ramase %d)", panel_q(5));
    ok = WAIT(pixel(task_x(0, 1) + 4, bar_cy()) == bar_bg, 3000);
    CHECK(ok, "bara fara butoane: %06x", pixel(task_x(0, 1) + 4, bar_cy()));
    msleep(300);

    /* --- 11. ypanel iese curat la SIGTERM, zona de lucru revine --- */
    kill(panel, SIGTERM);
    int stp = -1;
    waitpid(panel, &stp, 0);
    CHECK(WIFEXITED(stp) && WEXITSTATUS(stp) == 0, "ypanel iese curat la SIGTERM: %d", stp);
    ok = WAIT(panel_q(0) == 0, 3000);
    CHECK(ok, "fara panou zona de lucru revine (h %d)", panel_q(0));

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
    hold = access("/etc/panelhold", F_OK) == 0;
    test_panel();
    printf("paneltest: %d verificari, %d esecuri\n", checks, fails);
    if(fails){
        return 1;
    }
    printf("[OK] paneltest\n");
    return 0;
}
