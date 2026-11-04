/* yedit: editor de text in terminal, in stilul nano.
 *
 * Taste: sageti, Home/End (sau Ctrl-A/Ctrl-E), PgUp/PgDn (sau Ctrl-Y/Ctrl-V), Delete, Backspace, Enter, Tab;
 * Ctrl-S salveaza, Ctrl-X iese (cu confirmare daca sunt modificari), Ctrl-W cauta (Enter pe camp gol repeta ultima cautare),
 * Ctrl-G salt la linie, Ctrl-K taie linia, Ctrl-U lipeste, Ctrl-L redeseneaza.
 * Ecranul: randurile de sus = text, penultimul = bara de stare (fisier, pozitie), ultimul = mesaje / prompt.
 * Se deseneaza doar randurile care s-au schimbat (ESC [ r;c H, ESC [ K, ESC [ 7 m: cele suportate de consola).
 * Daca stdin nu e terminal, tastele se citesc din fisier (testele); la sfarsitul lor iese cu codul 1 fara sa salveze. */
#include "yedit.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define TABW 8
#define PROMPT_MAX 256
#define USAGE "uz: yedit [fisier]\n"

struct editor {
    struct gap g;
    size_t pos;                 /* cursorul: indice in text */
    long want_col;              /* coloana dorita la mutari pe verticala, -1 = coloana curenta */
    size_t topline;             /* prima linie vizibila */
    size_t leftcol;             /* prima coloana (de afisare) vizibila */
    int rows, cols, trows;      /* trows = randuri de text = rows - 2 */
    char *name;
    int modified;
    int last_cut;
    char *cut;
    size_t cutlen;
    char msg[160];
    char search[PROMPT_MAX];
    int prompt_on;
    char prompt_text[PROMPT_MAX + 64];
    char *frame, *shown;        /* ce vrem pe ecran / ce e pe ecran: rows * (cols + 1) */
    char *fattr, *sattr;        /* 1 = rand inversat (bara de stare) */
    int force_full;
};

static struct editor E;

static void quit_now(int code){
    term_close();
    out_str("\x1b[2J\x1b[H");
    out_flush();
    exit(code);
}

static void setmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setmsg(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof(E.msg), fmt, ap);
    va_end(ap);
}

/* ---------------- desenare ---------------- */

static int printable(char c){
    return c >= 0x20 && c < 0x7F;
}

/* Coloana de afisare a lui pos in linia lui (taburile se expandeaza la multiplu de 8) */
static size_t display_col(size_t pos){
    size_t col = 0;
    for(size_t p = gap_bol(&E.g, pos); p < pos; p++){
        col += gap_at(&E.g, p) == '\t' ? TABW - col % TABW : 1;
    }
    return col;
}

/* Linia care incepe la p, taiata la fereastra orizontala; intoarce sfarsitul liniei (pozitia lui '\n' sau len) */
static size_t build_line(char *row, size_t p){
    size_t len = gap_len(&E.g);
    size_t dc = 0;
    memset(row, ' ', (size_t)E.cols);
    while(p < len){
        char c = gap_at(&E.g, p);
        if(c == '\n'){
            break;
        }
        if(c == '\t'){
            dc += TABW - dc % TABW;
        } else {
            if(dc >= E.leftcol && dc - E.leftcol < (size_t)E.cols){
                row[dc - E.leftcol] = printable(c) ? c : '?';
            }
            dc++;
        }
        p++;
        if(dc >= E.leftcol + (size_t)E.cols){
            p = gap_eol(&E.g, p);
            break;
        }
    }
    return p;
}

static void set_row(int r, const char *s, int attr){
    char *row = E.frame + (size_t)r * (size_t)(E.cols + 1);
    size_t n = strlen(s);
    if(n > (size_t)E.cols){
        n = (size_t)E.cols;
    }
    memset(row, ' ', (size_t)E.cols);
    memcpy(row, s, n);
    E.fattr[r] = (char)attr;
}

static size_t total_lines(void){
    size_t len = gap_len(&E.g);
    size_t n = gap_lines_before(&E.g, len) + 1;
    if(len > 0 && gap_at(&E.g, len - 1) == '\n'){
        n--;
    }
    return n;
}

static void render(void){
    size_t len = gap_len(&E.g);
    if(E.pos > len){
        E.pos = len;
    }
    size_t bol = gap_bol(&E.g, E.pos);
    size_t curline = gap_lines_before(&E.g, E.pos);
    size_t dc = display_col(E.pos);
    if(curline < E.topline){
        E.topline = curline;
    } else if(curline >= E.topline + (size_t)E.trows){
        E.topline = curline - (size_t)E.trows + 1;
    }
    if(dc < E.leftcol){
        E.leftcol = dc;
    } else if(dc >= E.leftcol + (size_t)E.cols){
        E.leftcol = dc - (size_t)E.cols + 1;
    }

    /* inceputul primei linii vizibile: urcam din linia cursorului */
    size_t p = bol;
    for(size_t i = E.topline; i < curline; i++){
        p = gap_bol(&E.g, p - 1);
    }
    int have = 1;
    for(int r = 0; r < E.trows; r++){
        char *row = E.frame + (size_t)r * (size_t)(E.cols + 1);
        E.fattr[r] = 0;
        if(!have){
            memset(row, ' ', (size_t)E.cols);
            continue;
        }
        size_t e = build_line(row, p);
        if(e < len){
            p = e + 1;
        } else {
            have = 0;
        }
    }

    /* bara de stare */
    char left[400], right[64], bar[512];
    snprintf(left, sizeof(left), " yedit  %s%s", E.name ? E.name : "[fisier nou]", E.modified ? "  [modificat]" : "");
    size_t nl = total_lines();
    snprintf(right, sizeof(right), "Ln %lu/%lu  Col %lu ", (unsigned long)(curline + 1),
        (unsigned long)(nl > curline + 1 ? nl : curline + 1), (unsigned long)(E.pos - bol + 1));
    size_t rl = strlen(right);
    size_t ll = strlen(left);
    size_t cols = (size_t)E.cols;
    memset(bar, ' ', cols);
    if(ll + rl + 1 > cols){
        ll = cols > rl + 1 ? cols - rl - 1 : 0;
    }
    memcpy(bar, left, ll);
    if(rl <= cols){
        memcpy(bar + cols - rl, right, rl);
    }
    bar[cols] = 0;
    set_row(E.rows - 2, bar, 1);

    /* mesaj / prompt */
    if(E.prompt_on){
        set_row(E.rows - 1, E.prompt_text, 0);
    } else if(E.msg[0]){
        set_row(E.rows - 1, E.msg, 0);
    } else {
        set_row(E.rows - 1, "^S Salveaza  ^X Iesire  ^W Cauta  ^G Linia  ^K Taie  ^U Lipeste", 0);
    }

    /* doar randurile schimbate */
    char tmp[32];
    for(int r = 0; r < E.rows; r++){
        char *f = E.frame + (size_t)r * (size_t)(E.cols + 1);
        char *s = E.shown + (size_t)r * (size_t)(E.cols + 1);
        if(!E.force_full && E.fattr[r] == E.sattr[r] && memcmp(f, s, cols) == 0){
            continue;
        }
        snprintf(tmp, sizeof(tmp), "\x1b[%d;1H", r + 1);
        out_str(tmp);
        if(E.fattr[r]){
            out_str("\x1b[7m");
            out_bytes(f, cols);
            out_str("\x1b[m");
        } else {
            size_t n = cols;
            while(n > 0 && f[n - 1] == ' '){
                n--;
            }
            out_bytes(f, n);
            if(n < cols){
                out_str("\x1b[K");
            }
        }
        memcpy(s, f, cols);
        E.sattr[r] = E.fattr[r];
    }
    E.force_full = 0;

    int cr, cc;
    if(E.prompt_on){
        size_t pl = strlen(E.prompt_text);
        cr = E.rows;
        cc = (int)(pl < cols ? pl : cols - 1) + 1;
    } else {
        cr = (int)(curline - E.topline) + 1;
        cc = (int)(dc - E.leftcol) + 1;
    }
    snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH", cr, cc);
    out_str(tmp);
    out_flush();
}

/* ---------------- taste, prompturi ---------------- */

static int get_key(void){
    int k = term_key();
    if(k == K_EOF){
        quit_now(1);
    }
    return k;
}

/* 1 = Enter, 0 = anulat (Ctrl-C / Esc) */
static int prompt(const char *label, char *buf, size_t cap){
    size_t n = strlen(buf);
    for(;;){
        snprintf(E.prompt_text, sizeof(E.prompt_text), "%s%s", label, buf);
        E.prompt_on = 1;
        render();
        int k = get_key();
        if(k == '\n' || k == '\r'){
            E.prompt_on = 0;
            return 1;
        }
        if(k == 3 || k == K_ESC){
            E.prompt_on = 0;
            return 0;
        }
        if((k == 0x7F || k == 8) && n > 0){
            buf[--n] = 0;
        } else if(k >= 0x20 && k < 0x7F && n + 1 < cap){
            buf[n++] = (char)k;
            buf[n] = 0;
        }
    }
}

/* ---------------- fisier ---------------- */

/* 0 = ok (inclusiv fisier nou), -1 = eroare (mesaj pe stderr) */
static int load_file(const char *path){
    struct stat st;
    if(stat(path, &st) == 0 && S_ISDIR(st.st_mode)){
        fprintf(stderr, "yedit: %s: este director\n", path);
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        if(errno == ENOENT){
            setmsg("Fisier nou");
            return 0;
        }
        fprintf(stderr, "yedit: %s: %s\n", path, strerror(errno));
        return -1;
    }
    static char chunk[4096];
    for(;;){
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if(n < 0){
            fprintf(stderr, "yedit: %s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        if(n == 0){
            break;
        }
        if(gap_insert(&E.g, gap_len(&E.g), chunk, (size_t)n) < 0){
            fprintf(stderr, "yedit: %s: memorie insuficienta\n", path);
            close(fd);
            return -1;
        }
    }
    close(fd);
    setmsg("Citit %lu linii", (unsigned long)total_lines());
    return 0;
}

static int write_all(int fd, const char *p, size_t n){
    while(n > 0){
        ssize_t w = write(fd, p, n);
        if(w <= 0){
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 1 = salvat, 0 = nu (anulat sau eroare; mesajul e setat) */
static int save_file(void){
    if(!E.name){
        char buf[PROMPT_MAX] = "";
        if(!prompt("Nume fisier: ", buf, sizeof(buf)) || !buf[0]){
            setmsg("Salvare anulata");
            return 0;
        }
        E.name = strdup(buf);
        if(!E.name){
            setmsg("Memorie insuficienta");
            return 0;
        }
    }
    size_t len = gap_len(&E.g);
    if(len > 0 && gap_at(&E.g, len - 1) != '\n'){
        gap_insert(&E.g, len, "\n", 1);         /* fisierul se termina mereu cu newline */
        len++;
    }
    int fd = open(E.name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0){
        setmsg("Eroare la salvare: %s: %s", E.name, strerror(errno));
        return 0;
    }
    int bad = write_all(fd, E.g.buf, E.g.gs) < 0 || write_all(fd, E.g.buf + E.g.ge, E.g.cap - E.g.ge) < 0;
    int err = errno;
    if(close(fd) < 0){
        bad = 1;
        err = errno;
    }
    if(bad){
        setmsg("Eroare la scriere: %s", strerror(err));
        return 0;
    }
    E.modified = 0;
    setmsg("Salvat %lu linii, %lu octeti", (unsigned long)total_lines(), (unsigned long)len);
    return 1;
}

/* ---------------- editare ---------------- */

static void insert_text(const char *s, size_t n){
    if(gap_insert(&E.g, E.pos, s, n) < 0){
        setmsg("Memorie insuficienta");
        return;
    }
    E.pos += n;
    E.modified = 1;
}

static void move_up(void){
    size_t bol = gap_bol(&E.g, E.pos);
    if(bol == 0){
        return;
    }
    if(E.want_col < 0){
        E.want_col = (long)(E.pos - bol);
    }
    size_t ps = gap_bol(&E.g, bol - 1);
    size_t plen = bol - 1 - ps;
    E.pos = ps + ((size_t)E.want_col < plen ? (size_t)E.want_col : plen);
}

static void move_down(void){
    size_t len = gap_len(&E.g);
    size_t eol = gap_eol(&E.g, E.pos);
    if(eol >= len){
        return;
    }
    if(E.want_col < 0){
        E.want_col = (long)(E.pos - gap_bol(&E.g, E.pos));
    }
    size_t ns = eol + 1;
    size_t nlen = gap_eol(&E.g, ns) - ns;
    E.pos = ns + ((size_t)E.want_col < nlen ? (size_t)E.want_col : nlen);
}

static void cut_line(int append){
    size_t len = gap_len(&E.g);
    size_t bol = gap_bol(&E.g, E.pos);
    size_t eol = gap_eol(&E.g, E.pos);
    size_t end = eol < len ? eol + 1 : eol;
    size_t n = end - bol;
    if(n == 0){
        return;
    }
    size_t keep = append ? E.cutlen : 0;
    char *nc = realloc(E.cut, keep + n);
    if(!nc){
        setmsg("Memorie insuficienta");
        return;
    }
    E.cut = nc;
    gap_copy(&E.g, bol, n, E.cut + keep);
    E.cutlen = keep + n;
    gap_delete(&E.g, bol, n);
    E.pos = bol;
    E.modified = 1;
    E.last_cut = 1;
}

static int match_at(size_t s, const char *t, size_t m){
    for(size_t j = 0; j < m; j++){
        if(gap_at(&E.g, s + j) != t[j]){
            return 0;
        }
    }
    return 1;
}

static void do_search(void){
    char buf[PROMPT_MAX] = "";
    char label[PROMPT_MAX + 32];
    if(E.search[0]){
        snprintf(label, sizeof(label), "Cauta [%s]: ", E.search);
    } else {
        snprintf(label, sizeof(label), "Cauta: ");
    }
    if(!prompt(label, buf, sizeof(buf))){
        setmsg("Cautare anulata");
        return;
    }
    if(buf[0]){
        snprintf(E.search, sizeof(E.search), "%s", buf);
    }
    if(!E.search[0]){
        return;
    }
    size_t m = strlen(E.search);
    size_t len = gap_len(&E.g);
    if(m > len){
        setmsg("\"%s\" negasit", E.search);
        return;
    }
    for(size_t s = E.pos + 1; s + m <= len; s++){
        if(gap_at(&E.g, s) == E.search[0] && match_at(s, E.search, m)){
            E.pos = s;
            return;
        }
    }
    for(size_t s = 0; s <= E.pos && s + m <= len; s++){
        if(gap_at(&E.g, s) == E.search[0] && match_at(s, E.search, m)){
            E.pos = s;
            setmsg("Cautare reluata de la inceput");
            return;
        }
    }
    setmsg("\"%s\" negasit", E.search);
}

static void do_goto(void){
    char buf[PROMPT_MAX] = "";
    if(!prompt("Linia: ", buf, sizeof(buf)) || !buf[0]){
        return;
    }
    long n = atol(buf);
    if(n < 1){
        n = 1;
    }
    E.pos = gap_line_offset(&E.g, (size_t)n - 1);
}

/* 1 = se poate iesi */
static int do_exit(void){
    if(!E.modified){
        return 1;
    }
    setmsg("Salvezi modificarile? (d = da, n = nu, alta tasta = anulare)");
    render();
    int k = get_key();
    E.msg[0] = 0;
    if(k == 'd' || k == 'D' || k == 'y' || k == 'Y'){
        return save_file();
    }
    return k == 'n' || k == 'N';
}

static int page_lines(void){
    return E.trows > 1 ? E.trows - 1 : 1;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        fputs(USAGE, stdout);
        return 0;
    }
    if(argc > 2 || (argc == 2 && argv[1][0] == '-' && argv[1][1])){
        fputs(USAGE, stderr);
        return 2;
    }
    gap_init(&E.g);
    E.want_col = -1;
    if(argc == 2){
        E.name = strdup(argv[1]);
        if(!E.name || load_file(E.name) < 0){
            return 1;
        }
    }
    term_open();
    term_size(&E.rows, &E.cols);
    E.trows = E.rows - 2;
    size_t fb = (size_t)E.rows * (size_t)(E.cols + 1);
    E.frame = calloc(fb, 1);
    E.shown = calloc(fb, 1);
    E.fattr = calloc((size_t)E.rows, 1);
    E.sattr = calloc((size_t)E.rows, 1);
    if(!E.frame || !E.shown || !E.fattr || !E.sattr){
        term_close();
        fputs("yedit: memorie insuficienta\n", stderr);
        return 1;
    }
    E.force_full = 1;

    for(;;){
        render();
        int k = get_key();
        int was_cut = E.last_cut;
        int vertical = 0;
        E.last_cut = 0;
        E.msg[0] = 0;
        switch(k){
        case K_UP: move_up(); vertical = 1; break;
        case K_DOWN: move_down(); vertical = 1; break;
        case K_LEFT: if(E.pos > 0) E.pos--; break;
        case K_RIGHT: if(E.pos < gap_len(&E.g)) E.pos++; break;
        case K_HOME: case 0x01: E.pos = gap_bol(&E.g, E.pos); break;
        case K_END: case 0x05: E.pos = gap_eol(&E.g, E.pos); break;
        case K_PGUP: case 0x19:
            for(int i = 0; i < page_lines(); i++) move_up();
            vertical = 1;
            break;
        case K_PGDN: case 0x16:
            for(int i = 0; i < page_lines(); i++) move_down();
            vertical = 1;
            break;
        case K_DEL:
            if(E.pos < gap_len(&E.g)){
                gap_delete(&E.g, E.pos, 1);
                E.modified = 1;
            }
            break;
        case 0x7F: case 8:
            if(E.pos > 0){
                gap_delete(&E.g, E.pos - 1, 1);
                E.pos--;
                E.modified = 1;
            }
            break;
        case '\n': case '\r': insert_text("\n", 1); break;
        case '\t': insert_text("\t", 1); break;
        case 0x13: save_file(); break;                          /* Ctrl-S */
        case 0x18:                                              /* Ctrl-X */
            if(do_exit()){
                quit_now(0);
            }
            break;
        case 0x17: do_search(); break;                          /* Ctrl-W */
        case 0x07: do_goto(); break;                            /* Ctrl-G */
        case 0x0B: cut_line(was_cut); break;                    /* Ctrl-K */
        case 0x15:                                              /* Ctrl-U */
            if(E.cutlen){
                insert_text(E.cut, E.cutlen);
            }
            break;
        case 0x0C: E.force_full = 1; break;                     /* Ctrl-L */
        default:
            if(k >= 0x20 && k < 0x7F){
                char c = (char)k;
                insert_text(&c, 1);
            }
            break;
        }
        if(!vertical){
            E.want_col = -1;
        }
    }
}
