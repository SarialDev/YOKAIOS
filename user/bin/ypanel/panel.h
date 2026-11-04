#ifndef YPANEL_H
#define YPANEL_H
/* ypanel (T10.4): bara de jos a ecranului pentru ycomp. Centru: butonul Start (logo Yokai); stanga: aplicatii ancorate din
 * /etc/ypanel.conf; dreapta: ferestrele deschise (clic = focus / minimizare, clic dreapta = inchide), spatiul de lucru si
 * ceasul. Meniul Start (popup) are lista aplicatiilor si cautare. Vezi bar.c, menu.c, conf.c, launch.c, main.c. */
#include "ywin.h"
#include <stdint.h>

#define PANEL_H     40
#define MAX_APPS    48
#define MAX_TASKS   32
#define NAME_LEN    32
#define CMD_LEN     128
#define CONF_PATH   "/etc/ypanel.conf"

typedef struct {
    char name[NAME_LEN];
    char cmd[CMD_LEN];
    int pin;
} App;

typedef struct {
    uint32_t id;
    int flags;              /* YW_TF_* */
    int ws;
    char title[32];
} Task;

/* Ce e sub cursor in bara */
enum { H_NONE = 0, H_START, H_PIN, H_TASK };

typedef struct {
    YwConn *c;
    YwWindow *win;          /* bara */
    uint32_t win_id;
    YgFont *font, *bold;
    const YgTheme *th;
    int sw, sh;
    App apps[MAX_APPS];
    int napps;
    int pins[MAX_APPS];     /* indicii aplicatiilor ancorate, in ordinea din fisier */
    int npins;
    Task tasks[MAX_TASKS];
    int ntasks;
    int hover_kind, hover_idx;
    int ws, tilemask;       /* spatiul de lucru curent + masca modurilor (bit i = tiling) */
    char clock[8];          /* "HH:MM" */
    unsigned launches;
    unsigned events_task, events_start;
} Panel;

extern Panel P;

/* conf.c: citeste fisierul (sau valorile implicite daca lipseste); numarul de aplicatii */
int conf_load(const char *path);

/* launch.c */
int app_launch(const char *cmd);
void reap_children(void);

/* bar.c */
void bar_draw(void);
void bar_redraw(void);              /* bar_draw + damage */
void bar_hit(int x, int y, int *kind, int *idx);
YgRect bar_start_rect(void);
int bar_update_clock(void);         /* 1 daca textul s-a schimbat */
/* desen comun cu meniul */
void draw_icon(YgSurface *s, int x, int y, int size, const char *name, int on_accent);
void text_left(YgSurface *s, YgFont *f, int px, YgRect r, const char *str, YgColor col);

/* menu.c */
int menu_is_open(void);
uint32_t menu_window_id(void);
void menu_toggle(void);
void menu_close(void);
void menu_event(const YwEvent *ev);
int menu_visible_count(void);       /* aplicatii care trec de filtrul curent (teste) */

#endif
