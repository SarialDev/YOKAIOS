#ifndef YTERM_VT_H
#define YTERM_VT_H
/* Emulatorul de terminal al yterm (T10.5): un subset VT100 / xterm. Nu stie nimic de ferestre sau fisiere: primeste octeti
 * (vt_feed), tine grila de celule, cursorul, istoricul (scrollback) si raspunde aplicatiei prin callback-ul write.
 * Suportat: \r \n \b \t \a, ESC 7/8 D E M c, CSI A B C D E F G H f J K L M P @ X S T d ` e a m r s u n c, SGR (16 + 256 de culori,
 * bold, subliniat, invers; truecolor se reduce la paleta de 256), DECSET/DECRST 1 7 25 47 1047 1049 (alt screen),
 * regiune de derulare (DECSTBM), OSC 0/2 (titlu), UTF-8 (celule de latime 1). */
#include <stdint.h>

#define VT_DEFAULT   256
#define VT_A_BOLD    1
#define VT_A_UNDER   2
#define VT_A_REV     4
#define VT_A_DIM     8

typedef struct {
    uint32_t ch;            /* codepoint, 0 / ' ' = gol */
    uint16_t fg, bg;        /* 0..255 (paleta xterm) sau VT_DEFAULT */
    uint8_t attr;
} VtCell;

typedef struct {
    void (*write)(void *user, const char *s, int n);    /* raspuns catre aplicatie (DSR, DA) */
    void (*title)(void *user, const char *t);
    void (*bell)(void *user);
    void *user;
} VtOps;

typedef struct {
    VtCell *cells;          /* rows * cols */
    uint8_t *dirty;         /* per rand */
    int cols, rows;
    int x, y;               /* cursorul */
    int wrap_pending;
    int top, bottom;        /* regiunea de derulare (inclusiv) */
    uint16_t fg, bg;
    uint8_t attr;
    int sx, sy;             /* DECSC */
    uint16_t sfg, sbg;
    uint8_t sattr;
    int cursor_visible;
    int app_cursor;
    int autowrap;
    int alt;                /* ecranul alternativ activ */
    VtCell *saved_main;     /* ecranul principal cat timp alt = 1 */
    int main_x, main_y;
    /* istoric: ring de linii alocate */
    VtCell **sb;
    int *sb_len;
    int sb_cap, sb_n, sb_head;
    /* parser */
    int state;
    int params[16], nparams;
    int priv;               /* ? > = in CSI */
    char osc[128];
    int osc_len;
    uint32_t utf, utf_left;
    VtOps ops;
} Vt;

Vt *vt_new(int cols, int rows, int scrollback, const VtOps *ops);
void vt_free(Vt *v);
void vt_feed(Vt *v, const char *buf, int n);
void vt_resize(Vt *v, int cols, int rows);
void vt_mark_all_dirty(Vt *v);
/* Linia i din istoric (0 = cea mai veche); len = celule valide */
const VtCell *vt_sb_line(const Vt *v, int i, int *len);
/* Culoare RGB (0xRRGGBB) pentru un index din paleta de 256 */
uint32_t vt_palette(int idx);
#endif
