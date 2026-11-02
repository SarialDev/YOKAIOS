/* Editarea liniei in mod brut: sageti, Home/End/Delete, Backspace, istoric (sus/jos), Tab (cai si comenzi), Ctrl-A/E/K/U/W/L/C/D.
 * Terminalul (kernel, drivers/tty.cpp) trimite tastele ca octeti, sagetile ca ESC [ A..D. Pe tty-ul YokaiOS ecoul e oprit
 * cat timp citim o linie: ecranul il desenam noi (redraw) cu secvente ANSI (A B C D J K) */
#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <limits.h>

/* ---------------- istoric ---------------- */

#define HIST_MAX 100
static char *hist[HIST_MAX];
static int hist_n;

void history_add(const char *line){
    if(!*line || (hist_n && strcmp(hist[hist_n - 1], line) == 0)){
        return;
    }
    if(hist_n == HIST_MAX){
        free(hist[0]);
        memmove(hist, hist + 1, (HIST_MAX - 1) * sizeof(char *));
        hist_n--;
    }
    char *copy = strdup(line);
    if(copy){
        hist[hist_n++] = copy;
    }
}

void history_print(void){
    for(int i = 0; i < hist_n; i++){
        printf("%5d  %s\n", i + 1, hist[i]);
    }
}

int history_count(void){
    return hist_n;
}

/* ---------------- iesire cu buffer ---------------- */

static char ob[4096];
static size_t ob_len;

static void ob_flush(void){
    size_t off = 0;
    while(off < ob_len){
        ssize_t w = write(1, ob + off, ob_len - off);
        if(w <= 0){
            break;
        }
        off += (size_t)w;
    }
    ob_len = 0;
}

static void ob_put(char c){
    if(ob_len == sizeof(ob)){
        ob_flush();
    }
    ob[ob_len++] = c;
}

static void ob_bytes(const char *s, size_t n){
    for(size_t i = 0; i < n; i++){
        ob_put(s[i]);
    }
}

static void ob_str(const char *s){
    ob_bytes(s, strlen(s));
}

/* ESC [ n <final> */
static void ob_csi(int n, char final){
    char tmp[16];
    int len = snprintf(tmp, sizeof(tmp), "\x1b[%d%c", n, final);
    ob_bytes(tmp, (size_t)len);
}

/* ---------------- terminal ---------------- */

static struct termios saved_tio, raw_tio;

static void raw_enter(void){
    tcgetattr(0, &saved_tio);
    raw_tio = saved_tio;
    /* Fara ISIG: Ctrl-C ajunge aici ca octet (renunta la linie), nu ca SIGINT */
    raw_tio.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw_tio.c_cc[VMIN] = 1;
    raw_tio.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw_tio);
}

static void raw_leave(void){
    tcsetattr(0, TCSANOW, &saved_tio);
}

static void set_vmin(int v){
    raw_tio.c_cc[VMIN] = (cc_t)v;
    tcsetattr(0, TCSANOW, &raw_tio);
}

static int term_cols(void){
    struct winsize ws;
    if(ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20){
        return ws.ws_col;
    }
    return 80;
}

enum { K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_DEL, K_IGNORE };

/* Octetul de dupa ESC, in modul VMIN=0 (secventa vine intreaga, dar un ESC singur nu trebuie sa blocheze) */
static int esc_next(void){
    for(int tries = 0; tries < 4; tries++){
        unsigned char c;
        if(read(0, &c, 1) == 1){
            return c;
        }
        usleep(3000);
    }
    return -1;
}

static int parse_escape(void){
    int b = esc_next();
    if(b == '['){
        int params[2] = {0, 0};
        int np = 0;
        int ch;
        while((ch = esc_next()) >= 0){
            if(isdigit(ch)){
                params[np] = params[np] * 10 + (ch - '0');
            } else if(ch == ';'){
                if(np < 1){
                    np++;
                }
            } else {
                break;
            }
        }
        switch(ch){
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~':
            switch(params[0]){
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 3: return K_DEL;
            default: return K_IGNORE;
            }
        default: return K_IGNORE;
        }
    }
    if(b == 'O'){
        switch(esc_next()){
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        default: return K_IGNORE;
        }
    }
    return K_IGNORE;
}

/* Tasta urmatoare: octet (0..255), K_*; -1 la EOF/eroare */
static int getkey(void){
    unsigned char c;
    ssize_t n = read(0, &c, 1);
    if(n != 1){
        return -1;
    }
    if(c != 27){
        return c;
    }
    set_vmin(0);
    int k = parse_escape();
    set_vmin(1);
    return k;
}

/* ---------------- editor ---------------- */

struct ed {
    char *buf;
    size_t cap;
    int len, pos;
    int shown;              /* pozitia cursorului pe ecran, ca indice in buf */
    const char *prompt;
    int plen, cols;
};

/* Redeseneaza prompt-ul si linia de la randul de start si pune cursorul la e->pos */
static void redraw(struct ed *e){
    int old_row = (e->plen + e->shown) / e->cols;
    if(old_row > 0){
        ob_csi(old_row, 'A');
    }
    ob_put('\r');
    ob_str("\x1b[J");
    ob_str(e->prompt);
    ob_bytes(e->buf, (size_t)e->len);
    int total = e->plen + e->len;
    if(total % e->cols == 0){
        ob_put('\n');                       /* ultima coloana plina: cursorul trece pe randul urmator */
    }
    int end_row = total / e->cols;
    int tgt = e->plen + e->pos;
    int tgt_row = tgt / e->cols, tgt_col = tgt % e->cols;
    if(end_row > tgt_row){
        ob_csi(end_row - tgt_row, 'A');
    }
    ob_put('\r');
    if(tgt_col > 0){
        ob_csi(tgt_col, 'C');
    }
    e->shown = e->pos;
}

static void insert_at(struct ed *e, int at, const char *s, int n){
    if((size_t)(e->len + n) >= e->cap){
        n = (int)(e->cap - 1) - e->len;
    }
    if(n <= 0){
        return;
    }
    memmove(e->buf + at + n, e->buf + at, (size_t)(e->len - at));
    memcpy(e->buf + at, s, (size_t)n);
    e->len += n;
    e->buf[e->len] = 0;
    if(e->pos >= at){
        e->pos += n;
    }
}

static void delete_range(struct ed *e, int from, int to){
    if(to <= from){
        return;
    }
    memmove(e->buf + from, e->buf + to, (size_t)(e->len - to));
    e->len -= to - from;
    e->buf[e->len] = 0;
    if(e->pos > to){
        e->pos -= to - from;
    } else if(e->pos > from){
        e->pos = from;
    }
}

static void insert_char(struct ed *e, char c){
    int was_end = e->pos == e->len && e->shown == e->len;
    int before = e->len;
    insert_at(e, e->pos, &c, 1);
    if(e->len == before){
        return;
    }
    if(was_end){
        ob_put(c);
        e->shown = e->pos;
        if((e->plen + e->len) % e->cols == 0){
            ob_put('\n');
        }
    } else {
        redraw(e);
    }
}

static void backspace(struct ed *e){
    if(e->pos == 0){
        return;
    }
    int at_end = e->pos == e->len && e->shown == e->len;
    int boundary = (e->plen + e->len) % e->cols == 0;
    delete_range(e, e->pos - 1, e->pos);
    if(at_end && !boundary){
        ob_str("\b \b");
        e->shown = e->pos;
    } else {
        redraw(e);
    }
}

static void set_line(struct ed *e, const char *s){
    size_t n = strlen(s);
    if(n >= e->cap){
        n = e->cap - 1;
    }
    memcpy(e->buf, s, n);
    e->buf[n] = 0;
    e->len = e->pos = (int)n;
}

/* ---------------- completare ---------------- */

struct cands {
    char **v;
    int n, cap;
};

static void cands_add(struct cands *c, const char *name, int is_dir){
    size_t nl = strlen(name);
    char *s = malloc(nl + 2);
    if(!s){
        return;
    }
    memcpy(s, name, nl);
    s[nl] = 0;
    if(is_dir){
        s[nl] = '/';
        s[nl + 1] = 0;
    }
    for(int i = 0; i < c->n; i++){
        if(strcmp(c->v[i], s) == 0){          /* acelasi nume in doua directoare din PATH */
            free(s);
            return;
        }
    }
    if(c->n == c->cap){
        int ncap = c->cap ? c->cap * 2 : 32;
        char **nv = realloc(c->v, (size_t)ncap * sizeof(char *));
        if(!nv){
            free(s);
            return;
        }
        c->v = nv;
        c->cap = ncap;
    }
    c->v[c->n++] = s;
}

static void cands_free(struct cands *c){
    for(int i = 0; i < c->n; i++){
        free(c->v[i]);
    }
    free(c->v);
    c->v = NULL;
    c->n = c->cap = 0;
}

static int cmp_str(const void *a, const void *b){
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int path_is_dir(const char *dir, const char *name){
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    struct stat st;
    return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Intrarile din dir cu prefixul base; for_command: fara directoare (se cauta numai comenzi) */
static void collect_dir(struct cands *c, const char *dir, const char *base, int for_command){
    DIR *d = opendir(dir);
    if(!d){
        return;
    }
    size_t bl = strlen(base);
    struct dirent *de;
    while((de = readdir(d))){
        const char *nm = de->d_name;
        if(strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0){
            continue;
        }
        if(nm[0] == '.' && base[0] != '.'){
            continue;
        }
        if(strncmp(nm, base, bl) != 0){
            continue;
        }
        int is_dir = de->d_type == DT_DIR || ((de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) && path_is_dir(dir, nm));
        if(for_command && is_dir){
            continue;
        }
        cands_add(c, nm, is_dir);
    }
    closedir(d);
}

static int needs_escape(char c){
    return strchr(" \t'\"\\$&|;<>()*?#!`", c) != NULL;
}

/* Cuvantul de sub cursor: [ws, pos) */
static int word_start(struct ed *e){
    int ws = e->pos;
    while(ws > 0){
        char c = e->buf[ws - 1];
        if(isspace((unsigned char)c) && !(ws >= 2 && e->buf[ws - 2] == '\\')){
            break;
        }
        ws--;
    }
    return ws;
}

static void show_candidates(struct ed *e, struct cands *c){
    int saved_pos = e->pos;
    qsort(c->v, (size_t)c->n, sizeof(char *), cmp_str);
    e->pos = e->len;
    redraw(e);
    if((e->plen + e->len) % e->cols != 0){
        ob_put('\n');
    }
    size_t maxl = 0;
    for(int i = 0; i < c->n; i++){
        size_t l = strlen(c->v[i]);
        if(l > maxl){
            maxl = l;
        }
    }
    size_t colw = maxl + 2;
    int per_row = (int)((size_t)e->cols / colw);
    if(per_row < 1){
        per_row = 1;
    }
    for(int i = 0; i < c->n; i++){
        const char *s = c->v[i];
        ob_str(s);
        if((i + 1) % per_row == 0 || i + 1 == c->n){
            ob_put('\n');
        } else {
            for(size_t k = strlen(s); k < colw; k++){
                ob_put(' ');
            }
        }
    }
    e->pos = saved_pos;
    e->shown = 0;
    redraw(e);
}

static void complete(struct ed *e){
    int ws = word_start(e);
    /* Textul tastat, fara ghilimele si backslash-uri */
    char pre[PATH_MAX];
    int pn = 0;
    for(int i = ws; i < e->pos && pn < (int)sizeof(pre) - 1; i++){
        char c = e->buf[i];
        if(c == '\\' && i + 1 < e->pos){
            pre[pn++] = e->buf[++i];
        } else if(c != '\'' && c != '"'){
            pre[pn++] = c;
        }
    }
    pre[pn] = 0;

    int k = ws - 1;
    while(k >= 0 && isspace((unsigned char)e->buf[k])){
        k--;
    }
    int cmdpos = k < 0 || e->buf[k] == '|' || e->buf[k] == ';' || e->buf[k] == '&';

    struct cands c = {NULL, 0, 0};
    char dirpart[PATH_MAX] = "";
    const char *base = pre;
    int as_command = cmdpos && !strchr(pre, '/');
    if(as_command){
        size_t bl = strlen(pre);
        for(int i = 0; i < builtin_count(); i++){
            if(strncmp(builtin_name(i), pre, bl) == 0){
                cands_add(&c, builtin_name(i), 0);
            }
        }
        const char *path = var_get("PATH");
        if(!path){
            path = "/bin:/sbin";
        }
        while(*path){
            size_t n = strcspn(path, ":");
            char dir[PATH_MAX];
            if(n > 0 && n < sizeof(dir)){
                memcpy(dir, path, n);
                dir[n] = 0;
                collect_dir(&c, dir, pre, 1);
            }
            path += n + (path[n] ? 1 : 0);
        }
    } else {
        char *slash = strrchr(pre, '/');
        if(slash){
            size_t dl = (size_t)(slash - pre) + 1;
            memcpy(dirpart, pre, dl);
            dirpart[dl] = 0;
            base = slash + 1;
        }
        char open_dir[PATH_MAX];
        if(!*dirpart){
            strcpy(open_dir, ".");
        } else if(dirpart[0] == '~' && dirpart[1] == '/' && var_get("HOME")){
            snprintf(open_dir, sizeof(open_dir), "%s/%s", var_get("HOME"), dirpart + 2);
        } else {
            snprintf(open_dir, sizeof(open_dir), "%s", dirpart);
        }
        collect_dir(&c, open_dir, base, 0);
    }
    if(c.n == 0){
        ob_put('\a');
        cands_free(&c);
        return;
    }
    /* Prefixul comun al candidatilor */
    size_t common = strlen(c.v[0]);
    for(int i = 1; i < c.n; i++){
        size_t j = 0;
        while(j < common && c.v[i][j] == c.v[0][j]){
            j++;
        }
        common = j;
    }
    size_t base_len = strlen(base);
    int unique = c.n == 1;
    if(!unique && common <= base_len){
        show_candidates(e, &c);
        cands_free(&c);
        return;
    }
    /* Cuvantul nou: dirpart + prefixul comun (+ ' ' daca e unic si nu e director), cu caractere speciale escapate */
    char word[PATH_MAX * 2];
    size_t wn = 0;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s%.*s", dirpart, (int)common, c.v[0]);
    for(size_t i = 0; full[i] && wn + 3 < sizeof(word); i++){
        if(needs_escape(full[i])){
            word[wn++] = '\\';
        }
        word[wn++] = full[i];
    }
    if(unique && (common == 0 || c.v[0][common - 1] != '/')){
        word[wn++] = ' ';
    }
    word[wn] = 0;
    delete_range(e, ws, e->pos);
    e->pos = ws;
    insert_at(e, ws, word, (int)wn);
    cands_free(&c);
    redraw(e);
}

/* ---------------- citirea unei linii ---------------- */

int line_read(const char *prompt, char *out, size_t cap){
    if(!isatty(0)){
        // Intrare redirectata (ysh < script): fara prompt si fara editare
        if(!fgets(out, (int)cap, stdin)){
            return -1;
        }
        size_t n = strcspn(out, "\r\n");
        out[n] = 0;
        return (int)n;
    }
    struct ed e = {out, cap, 0, 0, 0, prompt, (int)strlen(prompt), term_cols()};
    out[0] = 0;
    int hist_idx = hist_n;
    char *saved_line = NULL;
    int result = 0;

    raw_enter();
    redraw(&e);
    for(;;){
        ob_flush();
        int k = getkey();
        if(k < 0){
            result = -1;
            break;
        }
        if(k == '\n' || k == '\r'){
            e.pos = e.len;
            if(e.shown != e.len){
                redraw(&e);
            }
            if((e.plen + e.len) % e.cols != 0){
                ob_put('\n');
            }
            result = e.len;
            break;
        }
        switch(k){
        case 127: case 8:
            backspace(&e);
            break;
        case K_DEL:
            if(e.pos < e.len){
                delete_range(&e, e.pos, e.pos + 1);
                redraw(&e);
            }
            break;
        case K_LEFT: case 2:
            if(e.pos > 0){
                e.pos--;
                if((e.plen + e.pos + 1) % e.cols != 0){
                    ob_str("\x1b[D");
                    e.shown = e.pos;
                } else {
                    redraw(&e);
                }
            }
            break;
        case K_RIGHT: case 6:
            if(e.pos < e.len){
                e.pos++;
                if((e.plen + e.pos) % e.cols != 0){
                    ob_str("\x1b[C");
                    e.shown = e.pos;
                } else {
                    redraw(&e);
                }
            }
            break;
        case K_HOME: case 1:
            e.pos = 0;
            redraw(&e);
            break;
        case K_END: case 5:
            e.pos = e.len;
            redraw(&e);
            break;
        case 11:                    /* Ctrl-K */
            delete_range(&e, e.pos, e.len);
            redraw(&e);
            break;
        case 21:                    /* Ctrl-U */
            delete_range(&e, 0, e.pos);
            e.pos = 0;
            redraw(&e);
            break;
        case 23: {                  /* Ctrl-W: cuvantul din stanga */
            int to = e.pos;
            while(to > 0 && isspace((unsigned char)e.buf[to - 1])){
                to--;
            }
            while(to > 0 && !isspace((unsigned char)e.buf[to - 1])){
                to--;
            }
            delete_range(&e, to, e.pos);
            e.pos = to;
            redraw(&e);
            break;
        }
        case 12:                    /* Ctrl-L */
            ob_str("\x1b[H\x1b[2J");
            e.shown = 0;
            redraw(&e);
            break;
        case 3:                     /* Ctrl-C */
            e.pos = e.len;
            if(e.shown != e.len){
                redraw(&e);
            }
            ob_str("^C\n");
            e.len = e.pos = 0;
            e.buf[0] = 0;
            result = 0;
            goto done;
        case 4:                     /* Ctrl-D */
            if(e.len == 0){
                result = -1;
                goto done;
            }
            if(e.pos < e.len){
                delete_range(&e, e.pos, e.pos + 1);
                redraw(&e);
            }
            break;
        case K_UP: case 16:
            if(hist_idx > 0){
                if(hist_idx == hist_n){
                    free(saved_line);
                    saved_line = strdup(e.buf);
                }
                hist_idx--;
                set_line(&e, hist[hist_idx]);
                redraw(&e);
            }
            break;
        case K_DOWN: case 14:
            if(hist_idx < hist_n){
                hist_idx++;
                set_line(&e, hist_idx == hist_n ? (saved_line ? saved_line : "") : hist[hist_idx]);
                redraw(&e);
            }
            break;
        case '\t':
            complete(&e);
            break;
        default:
            if(k >= 32 && k < 256 && k != 127){
                insert_char(&e, (char)k);
            }
            break;
        }
    }
done:
    ob_flush();
    free(saved_line);
    raw_leave();
    return result;
}
