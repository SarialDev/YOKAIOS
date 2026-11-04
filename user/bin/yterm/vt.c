/* Emulatorul de terminal (vezi vt.h) */
#include "vt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_SKIP_ONE, ST_SKIP_ST, ST_SKIP_ST_ESC };

static const uint8_t ansi16[16][3] = {
    {0, 0, 0}, {205, 49, 49}, {13, 188, 121}, {229, 229, 16}, {36, 114, 200}, {188, 63, 188}, {17, 168, 205}, {204, 204, 204},
    {102, 102, 102}, {241, 76, 76}, {35, 209, 139}, {245, 245, 67}, {59, 142, 234}, {214, 112, 214}, {41, 184, 219}, {255, 255, 255},
};

uint32_t vt_palette(int i){
    if(i < 0){
        i = 0;
    }
    if(i < 16){
        return ((uint32_t)ansi16[i][0] << 16) | ((uint32_t)ansi16[i][1] << 8) | ansi16[i][2];
    }
    if(i < 232){
        int c = i - 16;
        int r = c / 36, g = (c / 6) % 6, b = c % 6;
        int lv[6] = {0, 95, 135, 175, 215, 255};
        return ((uint32_t)lv[r] << 16) | ((uint32_t)lv[g] << 8) | (uint32_t)lv[b];
    }
    int gray = 8 + (i - 232) * 10;
    if(gray > 255){
        gray = 255;
    }
    return ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
}

static int rgb_to_256(int r, int g, int b){
    int lv[6] = {0, 95, 135, 175, 215, 255};
    int best = 16, bd = 1 << 30;
    for(int ri = 0; ri < 6; ri++){
        for(int gi = 0; gi < 6; gi++){
            for(int bi = 0; bi < 6; bi++){
                int d = (r - lv[ri]) * (r - lv[ri]) + (g - lv[gi]) * (g - lv[gi]) + (b - lv[bi]) * (b - lv[bi]);
                if(d < bd){
                    bd = d;
                    best = 16 + 36 * ri + 6 * gi + bi;
                }
            }
        }
    }
    int avg = (r + g + b) / 3;
    int gi = (avg - 8 + 5) / 10;
    if(gi < 0){
        gi = 0;
    }
    if(gi > 23){
        gi = 23;
    }
    int gv = 8 + gi * 10;
    if((r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv) < bd){
        best = 232 + gi;
    }
    return best;
}

static VtCell blank(const Vt *v){
    VtCell c;
    c.ch = ' ';
    c.fg = VT_DEFAULT;
    c.bg = v->bg;
    c.attr = 0;
    return c;
}

static VtCell *cell(const Vt *v, int r, int c){
    return &v->cells[r * v->cols + c];
}

static void clear_cells(Vt *v, int r, int c0, int c1){       /* [c0, c1) */
    VtCell b = blank(v);
    for(int c = c0; c < c1; c++){
        *cell(v, r, c) = b;
    }
    v->dirty[r] = 1;
}

static void sb_push(Vt *v, int row){
    if(v->sb_cap <= 0 || v->alt){
        return;
    }
    int slot;
    if(v->sb_n < v->sb_cap){
        slot = (v->sb_head + v->sb_n) % v->sb_cap;
        v->sb_n++;
    } else {
        slot = v->sb_head;
        v->sb_head = (v->sb_head + 1) % v->sb_cap;
        free(v->sb[slot]);
    }
    v->sb[slot] = malloc((size_t)v->cols * sizeof(VtCell));
    if(!v->sb[slot]){
        v->sb_len[slot] = 0;
        return;
    }
    memcpy(v->sb[slot], cell(v, row, 0), (size_t)v->cols * sizeof(VtCell));
    v->sb_len[slot] = v->cols;
}

const VtCell *vt_sb_line(const Vt *v, int i, int *len){
    if(i < 0 || i >= v->sb_n){
        *len = 0;
        return NULL;
    }
    int slot = (v->sb_head + i) % v->sb_cap;
    *len = v->sb_len[slot];
    return v->sb[slot];
}

static void scroll_up(Vt *v, int n){
    int top = v->top, bot = v->bottom;
    if(n > bot - top + 1){
        n = bot - top + 1;
    }
    if(top == 0){
        for(int i = 0; i < n; i++){
            sb_push(v, i);
        }
    }
    memmove(cell(v, top, 0), cell(v, top + n, 0), (size_t)(bot - top + 1 - n) * (size_t)v->cols * sizeof(VtCell));
    for(int r = bot - n + 1; r <= bot; r++){
        clear_cells(v, r, 0, v->cols);
    }
    for(int r = top; r <= bot; r++){
        v->dirty[r] = 1;
    }
}

static void scroll_down(Vt *v, int n){
    int top = v->top, bot = v->bottom;
    if(n > bot - top + 1){
        n = bot - top + 1;
    }
    memmove(cell(v, top + n, 0), cell(v, top, 0), (size_t)(bot - top + 1 - n) * (size_t)v->cols * sizeof(VtCell));
    for(int r = top; r < top + n; r++){
        clear_cells(v, r, 0, v->cols);
    }
    for(int r = top; r <= bot; r++){
        v->dirty[r] = 1;
    }
}

static void linefeed(Vt *v){
    if(v->y == v->bottom){
        scroll_up(v, 1);
    } else if(v->y < v->rows - 1){
        v->y++;
    }
}

static void reverse_index(Vt *v){
    if(v->y == v->top){
        scroll_down(v, 1);
    } else if(v->y > 0){
        v->y--;
    }
}

static void put_cp(Vt *v, uint32_t cp){
    if(v->wrap_pending && v->autowrap){
        v->x = 0;
        linefeed(v);
    }
    v->wrap_pending = 0;
    VtCell *c = cell(v, v->y, v->x);
    c->ch = cp;
    c->fg = v->fg;
    c->bg = v->bg;
    c->attr = v->attr;
    v->dirty[v->y] = 1;
    if(v->x == v->cols - 1){
        v->wrap_pending = 1;
    } else {
        v->x++;
    }
}

static void clampxy(Vt *v){
    if(v->x < 0){
        v->x = 0;
    }
    if(v->x >= v->cols){
        v->x = v->cols - 1;
    }
    if(v->y < 0){
        v->y = 0;
    }
    if(v->y >= v->rows){
        v->y = v->rows - 1;
    }
}

static int par(const Vt *v, int i, int def){
    if(i >= v->nparams || v->params[i] <= 0){
        return def;
    }
    return v->params[i];
}

static void reply(Vt *v, const char *s){
    if(v->ops.write){
        v->ops.write(v->ops.user, s, (int)strlen(s));
    }
}

static void sgr(Vt *v){
    if(v->nparams == 0){
        v->nparams = 1;
        v->params[0] = 0;
    }
    for(int i = 0; i < v->nparams; i++){
        int p = v->params[i] < 0 ? 0 : v->params[i];
        if(p == 0){
            v->fg = v->bg = VT_DEFAULT;
            v->attr = 0;
        } else if(p == 1){
            v->attr |= VT_A_BOLD;
        } else if(p == 2){
            v->attr |= VT_A_DIM;
        } else if(p == 4){
            v->attr |= VT_A_UNDER;
        } else if(p == 7){
            v->attr |= VT_A_REV;
        } else if(p == 22){
            v->attr &= (uint8_t) ~(VT_A_BOLD | VT_A_DIM);
        } else if(p == 24){
            v->attr &= (uint8_t)~VT_A_UNDER;
        } else if(p == 27){
            v->attr &= (uint8_t)~VT_A_REV;
        } else if(p >= 30 && p <= 37){
            v->fg = (uint16_t)(p - 30);
        } else if(p == 39){
            v->fg = VT_DEFAULT;
        } else if(p >= 40 && p <= 47){
            v->bg = (uint16_t)(p - 40);
        } else if(p == 49){
            v->bg = VT_DEFAULT;
        } else if(p >= 90 && p <= 97){
            v->fg = (uint16_t)(p - 90 + 8);
        } else if(p >= 100 && p <= 107){
            v->bg = (uint16_t)(p - 100 + 8);
        } else if(p == 38 || p == 48){
            int col = -1;
            if(i + 2 < v->nparams && v->params[i + 1] == 5){
                col = v->params[i + 2] & 255;
                i += 2;
            } else if(i + 4 < v->nparams && v->params[i + 1] == 2){
                col = rgb_to_256(v->params[i + 2] & 255, v->params[i + 3] & 255, v->params[i + 4] & 255);
                i += 4;
            }
            if(col >= 0){
                if(p == 38){
                    v->fg = (uint16_t)col;
                } else {
                    v->bg = (uint16_t)col;
                }
            }
        }
    }
}

static void alt_screen(Vt *v, int on, int save_cursor){
    if(on == v->alt){
        return;
    }
    size_t n = (size_t)v->cols * (size_t)v->rows * sizeof(VtCell);
    if(on){
        v->saved_main = malloc(n);
        if(!v->saved_main){
            return;
        }
        memcpy(v->saved_main, v->cells, n);
        v->main_x = v->x;
        v->main_y = v->y;
        v->alt = 1;
        v->fg = v->bg = VT_DEFAULT;
        v->attr = 0;
        for(int r = 0; r < v->rows; r++){
            clear_cells(v, r, 0, v->cols);
        }
        (void)save_cursor;
    } else {
        memcpy(v->cells, v->saved_main, n);
        free(v->saved_main);
        v->saved_main = NULL;
        v->alt = 0;
        v->x = v->main_x;
        v->y = v->main_y;
        v->top = 0;
        v->bottom = v->rows - 1;
        vt_mark_all_dirty(v);
    }
    v->wrap_pending = 0;
}

static void dec_mode(Vt *v, int set){
    for(int i = 0; i < v->nparams; i++){
        switch(v->params[i]){
        case 1: v->app_cursor = set; break;
        case 7: v->autowrap = set; break;
        case 25: v->cursor_visible = set; break;
        case 47: case 1047: alt_screen(v, set, 0); break;
        case 1049: alt_screen(v, set, 1); break;
        default: break;
        }
    }
}

static void erase_display(Vt *v, int mode){
    if(mode == 0){
        clear_cells(v, v->y, v->x, v->cols);
        for(int r = v->y + 1; r < v->rows; r++){
            clear_cells(v, r, 0, v->cols);
        }
    } else if(mode == 1){
        for(int r = 0; r < v->y; r++){
            clear_cells(v, r, 0, v->cols);
        }
        clear_cells(v, v->y, 0, v->x + 1);
    } else if(mode == 2 || mode == 3){
        for(int r = 0; r < v->rows; r++){
            clear_cells(v, r, 0, v->cols);
        }
        if(mode == 3){
            for(int i = 0; i < v->sb_n; i++){
                free(v->sb[(v->sb_head + i) % v->sb_cap]);
            }
            v->sb_n = 0;
            v->sb_head = 0;
        }
    }
}

static void csi(Vt *v, char f){
    int n;
    switch(f){
    case 'A': v->y -= par(v, 0, 1); if(v->y < v->top && v->y + par(v, 0, 1) >= v->top) v->y = v->top; clampxy(v); v->wrap_pending = 0; break;
    case 'B': v->y += par(v, 0, 1); if(v->y > v->bottom && v->y - par(v, 0, 1) <= v->bottom) v->y = v->bottom; clampxy(v); v->wrap_pending = 0; break;
    case 'C': case 'a': v->x += par(v, 0, 1); clampxy(v); v->wrap_pending = 0; break;
    case 'D': v->x -= par(v, 0, 1); clampxy(v); v->wrap_pending = 0; break;
    case 'E': v->x = 0; v->y += par(v, 0, 1); clampxy(v); v->wrap_pending = 0; break;
    case 'F': v->x = 0; v->y -= par(v, 0, 1); clampxy(v); v->wrap_pending = 0; break;
    case 'G': case '`': v->x = par(v, 0, 1) - 1; clampxy(v); v->wrap_pending = 0; break;
    case 'd': v->y = par(v, 0, 1) - 1; clampxy(v); v->wrap_pending = 0; break;
    case 'e': v->y += par(v, 0, 1); clampxy(v); v->wrap_pending = 0; break;
    case 'H': case 'f':
        v->y = par(v, 0, 1) - 1;
        v->x = par(v, 1, 1) - 1;
        clampxy(v);
        v->wrap_pending = 0;
        break;
    case 'J': erase_display(v, v->nparams ? v->params[0] : 0); break;
    case 'K': {
        int m = v->nparams ? v->params[0] : 0;
        if(m == 0){
            clear_cells(v, v->y, v->x, v->cols);
        } else if(m == 1){
            clear_cells(v, v->y, 0, v->x + 1);
        } else if(m == 2){
            clear_cells(v, v->y, 0, v->cols);
        }
        break;
    }
    case 'L':
        if(v->y >= v->top && v->y <= v->bottom){
            int save = v->top;
            v->top = v->y;
            scroll_down(v, par(v, 0, 1));
            v->top = save;
        }
        break;
    case 'M':
        if(v->y >= v->top && v->y <= v->bottom){
            int save = v->top;
            v->top = v->y;
            scroll_up(v, par(v, 0, 1));
            v->top = save;
        }
        break;
    case 'P':
        n = par(v, 0, 1);
        if(n > v->cols - v->x){
            n = v->cols - v->x;
        }
        memmove(cell(v, v->y, v->x), cell(v, v->y, v->x + n), (size_t)(v->cols - v->x - n) * sizeof(VtCell));
        clear_cells(v, v->y, v->cols - n, v->cols);
        break;
    case '@':
        n = par(v, 0, 1);
        if(n > v->cols - v->x){
            n = v->cols - v->x;
        }
        memmove(cell(v, v->y, v->x + n), cell(v, v->y, v->x), (size_t)(v->cols - v->x - n) * sizeof(VtCell));
        clear_cells(v, v->y, v->x, v->x + n);
        break;
    case 'X':
        n = par(v, 0, 1);
        if(n > v->cols - v->x){
            n = v->cols - v->x;
        }
        clear_cells(v, v->y, v->x, v->x + n);
        break;
    case 'S': scroll_up(v, par(v, 0, 1)); break;
    case 'T': scroll_down(v, par(v, 0, 1)); break;
    case 'm': sgr(v); break;
    case 'r': {
        int t = par(v, 0, 1) - 1, b = par(v, 1, v->rows) - 1;
        if(t < 0) t = 0;
        if(b >= v->rows) b = v->rows - 1;
        if(t < b){
            v->top = t;
            v->bottom = b;
            v->x = v->y = 0;
            v->wrap_pending = 0;
        }
        break;
    }
    case 's': v->sx = v->x; v->sy = v->y; break;
    case 'u': v->x = v->sx; v->y = v->sy; clampxy(v); v->wrap_pending = 0; break;
    case 'h': if(v->priv == '?') dec_mode(v, 1); break;
    case 'l': if(v->priv == '?') dec_mode(v, 0); break;
    case 'n':
        if(v->nparams && v->params[0] == 6){
            char b[32];
            snprintf(b, sizeof(b), "\x1b[%d;%dR", v->y + 1, v->x + 1);
            reply(v, b);
        } else if(v->nparams && v->params[0] == 5){
            reply(v, "\x1b[0n");
        }
        break;
    case 'c':
        if(v->priv != '>'){
            reply(v, "\x1b[?1;2c");
        }
        break;
    default:
        break;
    }
}

static void do_esc(Vt *v, char c){
    switch(c){
    case '[':
        v->state = ST_CSI;
        v->nparams = 0;
        v->priv = 0;
        memset(v->params, 0, sizeof(v->params));
        v->params[0] = -1;
        return;
    case ']':
        v->state = ST_OSC;
        v->osc_len = 0;
        return;
    case 'P': case '^': case '_': case 'X':
        v->state = ST_SKIP_ST;
        return;
    case '(': case ')': case '*': case '+': case '#': case '%':
        v->state = ST_SKIP_ONE;
        return;
    case '7': v->sx = v->x; v->sy = v->y; v->sfg = v->fg; v->sbg = v->bg; v->sattr = v->attr; break;
    case '8': v->x = v->sx; v->y = v->sy; v->fg = v->sfg; v->bg = v->sbg; v->attr = v->sattr; clampxy(v); v->wrap_pending = 0; break;
    case 'D': linefeed(v); break;
    case 'E': v->x = 0; linefeed(v); break;
    case 'M': reverse_index(v); break;
    case 'c':
        v->fg = v->bg = VT_DEFAULT;
        v->attr = 0;
        v->top = 0;
        v->bottom = v->rows - 1;
        v->x = v->y = 0;
        v->autowrap = 1;
        v->cursor_visible = 1;
        v->app_cursor = 0;
        for(int r = 0; r < v->rows; r++){
            clear_cells(v, r, 0, v->cols);
        }
        break;
    default:
        break;
    }
    v->state = ST_GROUND;
}

static void csi_byte(Vt *v, uint8_t c){
    if(c >= '0' && c <= '9'){
        if(v->nparams == 0){
            v->nparams = 1;
            v->params[0] = 0;
        }
        int i = v->nparams - 1;
        if(v->params[i] < 0){
            v->params[i] = 0;
        }
        v->params[i] = v->params[i] * 10 + (c - '0');
        if(v->params[i] > 100000){
            v->params[i] = 100000;
        }
        return;
    }
    if(c == ';'){
        if(v->nparams == 0){
            v->nparams = 1;
            v->params[0] = 0;
        }
        if(v->nparams < 16){
            v->params[v->nparams] = -1;
            v->nparams++;
        }
        return;
    }
    if(c == '?' || c == '>' || c == '=' || c == '<'){
        v->priv = c;
        return;
    }
    if(c >= 0x20 && c <= 0x2F){
        return;                         /* intermediari: ignorati */
    }
    if(c >= 0x40 && c <= 0x7E){
        for(int i = 0; i < v->nparams; i++){
            if(v->params[i] < 0){
                v->params[i] = 0;
            }
        }
        csi(v, (char)c);
    }
    v->state = ST_GROUND;
}

static void control(Vt *v, uint8_t c){
    switch(c){
    case '\r': v->x = 0; v->wrap_pending = 0; break;
    case '\n': case '\v': case '\f': linefeed(v); v->wrap_pending = 0; break;
    case '\b': if(v->x > 0) v->x--; v->wrap_pending = 0; break;
    case '\t': {
        int nx = (v->x / 8 + 1) * 8;
        v->x = nx >= v->cols ? v->cols - 1 : nx;
        v->wrap_pending = 0;
        break;
    }
    case 7: if(v->ops.bell) v->ops.bell(v->ops.user); break;
    default: break;
    }
}

static void feed_byte(Vt *v, uint8_t c){
    switch(v->state){
    case ST_ESC:
        if(c == 0x1B){
            return;
        }
        if(c < 0x20){
            control(v, c);
            return;
        }
        do_esc(v, (char)c);
        return;
    case ST_CSI:
        if(c == 0x1B){
            v->state = ST_ESC;
            return;
        }
        if(c < 0x20){
            control(v, c);
            return;
        }
        csi_byte(v, c);
        return;
    case ST_OSC:
        if(c == 7){
            goto osc_end;
        }
        if(c == 0x1B){
            v->state = ST_OSC_ESC;
            return;
        }
        if(v->osc_len < (int)sizeof(v->osc) - 1){
            v->osc[v->osc_len++] = (char)c;
        }
        return;
    case ST_OSC_ESC:
        if(c != '\\'){
            v->state = ST_GROUND;
            return;
        }
    osc_end:
        v->osc[v->osc_len] = 0;
        if(v->osc_len > 2 && (v->osc[0] == '0' || v->osc[0] == '2') && v->osc[1] == ';' && v->ops.title){
            v->ops.title(v->ops.user, v->osc + 2);
        }
        v->state = ST_GROUND;
        return;
    case ST_SKIP_ONE:
        v->state = ST_GROUND;
        return;
    case ST_SKIP_ST:
        if(c == 7){
            v->state = ST_GROUND;
        } else if(c == 0x1B){
            v->state = ST_SKIP_ST_ESC;
        }
        return;
    case ST_SKIP_ST_ESC:
        v->state = c == '\\' ? ST_GROUND : ST_SKIP_ST;
        return;
    default:
        break;
    }
    /* GROUND */
    if(v->utf_left){
        if((c & 0xC0) == 0x80){
            v->utf = (v->utf << 6) | (c & 0x3F);
            if(--v->utf_left == 0){
                put_cp(v, v->utf);
            }
            return;
        }
        v->utf_left = 0;
        put_cp(v, '?');
    }
    if(c == 0x1B){
        v->state = ST_ESC;
    } else if(c < 0x20){
        control(v, c);
    } else if(c < 0x7F){
        put_cp(v, c);
    } else if(c >= 0xC2 && c < 0xE0){
        v->utf = c & 0x1F;
        v->utf_left = 1;
    } else if(c >= 0xE0 && c < 0xF0){
        v->utf = c & 0x0F;
        v->utf_left = 2;
    } else if(c >= 0xF0 && c < 0xF5){
        v->utf = c & 0x07;
        v->utf_left = 3;
    }
}

void vt_feed(Vt *v, const char *buf, int n){
    for(int i = 0; i < n; i++){
        feed_byte(v, (uint8_t)buf[i]);
    }
}

void vt_mark_all_dirty(Vt *v){
    for(int r = 0; r < v->rows; r++){
        v->dirty[r] = 1;
    }
}

Vt *vt_new(int cols, int rows, int scrollback, const VtOps *ops){
    Vt *v = calloc(1, sizeof(*v));
    if(!v){
        return NULL;
    }
    v->cols = cols;
    v->rows = rows;
    v->cells = malloc((size_t)cols * (size_t)rows * sizeof(VtCell));
    v->dirty = calloc((size_t)rows, 1);
    v->sb_cap = scrollback;
    if(scrollback > 0){
        v->sb = calloc((size_t)scrollback, sizeof(VtCell *));
        v->sb_len = calloc((size_t)scrollback, sizeof(int));
    }
    if(!v->cells || !v->dirty || (scrollback > 0 && (!v->sb || !v->sb_len))){
        vt_free(v);
        return NULL;
    }
    if(ops){
        v->ops = *ops;
    }
    v->fg = v->bg = VT_DEFAULT;
    v->bottom = rows - 1;
    v->cursor_visible = 1;
    v->autowrap = 1;
    for(int r = 0; r < rows; r++){
        clear_cells(v, r, 0, cols);
    }
    return v;
}

void vt_free(Vt *v){
    if(!v){
        return;
    }
    for(int i = 0; i < v->sb_n; i++){
        free(v->sb[(v->sb_head + i) % v->sb_cap]);
    }
    free(v->sb);
    free(v->sb_len);
    free(v->saved_main);
    free(v->cells);
    free(v->dirty);
    free(v);
}

void vt_resize(Vt *v, int cols, int rows){
    if(cols < 1 || rows < 1 || (cols == v->cols && rows == v->rows)){
        return;
    }
    VtCell *nc = malloc((size_t)cols * (size_t)rows * sizeof(VtCell));
    uint8_t *nd = calloc((size_t)rows, 1);
    VtCell *ns = v->saved_main ? malloc((size_t)cols * (size_t)rows * sizeof(VtCell)) : NULL;
    if(!nc || !nd || (v->saved_main && !ns)){
        free(nc);
        free(nd);
        free(ns);
        return;
    }
    VtCell b = blank(v);
    for(size_t i = 0; i < (size_t)cols * (size_t)rows; i++){
        nc[i] = b;
        if(ns){
            ns[i] = b;
        }
    }
    int shift = 0;
    if(v->y >= rows){
        shift = v->y - rows + 1;
        if(!v->alt){
            for(int r = 0; r < shift; r++){
                sb_push(v, r);
            }
        }
    }
    int ccols = cols < v->cols ? cols : v->cols;
    for(int r = 0; r < rows; r++){
        int sr = r + shift;
        if(sr >= v->rows){
            break;
        }
        memcpy(nc + (size_t)r * (size_t)cols, v->cells + (size_t)sr * (size_t)v->cols, (size_t)ccols * sizeof(VtCell));
        if(ns){
            memcpy(ns + (size_t)r * (size_t)cols, v->saved_main + (size_t)sr * (size_t)v->cols, (size_t)ccols * sizeof(VtCell));
        }
    }
    free(v->cells);
    free(v->dirty);
    free(v->saved_main);
    v->cells = nc;
    v->dirty = nd;
    v->saved_main = ns;
    v->cols = cols;
    v->rows = rows;
    v->y -= shift;
    if(v->y < 0){
        v->y = 0;
    }
    v->top = 0;
    v->bottom = rows - 1;
    clampxy(v);
    v->wrap_pending = 0;
    vt_mark_all_dirty(v);
}
