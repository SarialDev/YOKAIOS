/* yfiles (T10.5): manager de fisiere grafic pentru ycomp.
 *   yfiles [-s socket] [-t dark|light] [director]
 * Lista cu directoare intai (nume, dimensiune, drepturi), locuri in stanga (Acasa, /, /tmp, /etc, /media), bara de unelte,
 * meniu pe clic dreapta. Mouse: clic selecteaza (Ctrl = adauga, Shift = interval), dublu clic deschide, rotita deruleaza.
 * Tastatura: sageti / PgUp / PgDn / Home / End, Enter = deschide, Backspace = director parinte, F2 = redenumeste, Delete = sterge
 * (cu confirmare), F4 = editor, F5 / Ctrl+R = reimprospateaza, Ctrl+C / X / V = copiaza / taie / lipeste, Ctrl+N = dosar nou,
 * Ctrl+Shift+N = fisier nou, Ctrl+L = mergi la cale, Ctrl+H = fisiere ascunse, Ctrl+A = selecteaza tot, litere = salt la nume.
 * Copierea / mutarea / stergerea ruleaza /bin/cp -r, /bin/mv, /bin/rm -rf. Fisierele text se deschid in `yterm -e yedit`,
 * programele (x) in `yterm -e program`. */
#include "ywin.h"
#include "yui.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/yinput.h>

#define TOOL_H   40
#define PATH_H   26
#define STATUS_H 24
#define SIDE_W   130
#define HEAD_H   22
#define ROW_H    24
#define PATH_MAX_Y 512

enum { K_ESC = 1, K_BS = 14, K_ENTER = 28, K_F2 = 59, K_F4 = 61, K_F5 = 62, K_HOME = 72, K_UP = 73, K_PGUP = 74, K_END = 77,
       K_DOWN = 78, K_PGDN = 79, K_DEL = 81 };

typedef struct {
    char name[128];
    int dir;
    long long size;
    unsigned mode;
} Ent;

enum { A_NONE = 0, A_UP, A_HOME, A_MKDIR, A_MKFILE, A_COPY, A_CUT, A_PASTE, A_DELETE, A_RENAME, A_EDIT, A_TERM, A_OPEN, A_HIDDEN,
       A_REFRESH, A_GOTO };

enum { M_NORMAL, M_PROMPT, M_CONFIRM, M_MENU };

static YwConn *conn;
static YwWindow *win;
static YUi ui;
static YgFont *font, *bold;
static const YgTheme *th = &yg_theme_dark;

static char cwd[PATH_MAX_Y] = "/";
static Ent *ents;
static unsigned char *sel;
static int nents, capents;
static int cur = -1, anchor = -1, scroll;
static int show_hidden;
static int hover_kind, hover_idx = -1;          /* 1 = unealta, 2 = loc, 3 = rand, 4 = element de meniu */
static char status[200];
static int mode = M_NORMAL;
static int dirty = 1;
static volatile sig_atomic_t child_flag;

/* clipboard */
static char *clip[64];
static int nclip, clip_cut;

/* prompt */
static int prompt_kind;                         /* A_RENAME / A_MKDIR / A_MKFILE / A_GOTO */
static char prompt_buf[256];
static int prompt_len;
/* meniu */
static int menu_x, menu_y;
static int last_click_idx = -1;
static long long last_click_ms;
static char places[6][64];
static const char *place_names[6];
static int nplaces;
static char typed[32];
static long long typed_ms;

static long long now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void set_status(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    dirty = 1;
}

static void join(char *out, size_t cap, const char *dir, const char *name){
    if(!strcmp(dir, "/")){
        snprintf(out, cap, "/%s", name);
    } else {
        snprintf(out, cap, "%s/%s", dir, name);
    }
}

static int ent_cmp(const void *a, const void *b){
    const Ent *x = a, *y = b;
    if(x->dir != y->dir){
        return y->dir - x->dir;
    }
    return strcasecmp(x->name, y->name);
}

static void fmt_size(long long n, char *out, size_t cap){
    if(n < 1024){
        snprintf(out, cap, "%lld B", n);
    } else if(n < 1024 * 1024){
        snprintf(out, cap, "%lld.%lld KiB", n / 1024, (n % 1024) * 10 / 1024);
    } else if(n < 1024LL * 1024 * 1024){
        snprintf(out, cap, "%lld.%lld MiB", n / (1024 * 1024), (n % (1024 * 1024)) * 10 / (1024 * 1024));
    } else {
        snprintf(out, cap, "%lld.%lld GiB", n / (1024LL * 1024 * 1024), (n % (1024LL * 1024 * 1024)) * 10 / (1024LL * 1024 * 1024));
    }
}

static void fmt_mode(unsigned m, char *out){
    const char *rwx = "rwxrwxrwx";
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
    for(int i = 0; i < 9; i++){
        out[1 + i] = m & (0400 >> i) ? rwx[i] : '-';
    }
    out[10] = 0;
}

static void clear_sel(void){
    if(sel){
        memset(sel, 0, (size_t)(nents > 0 ? nents : 0));
    }
}

static unsigned long long cur_sig;
static unsigned long long dir_sig(void);

/* Citeste directorul curent; pastreaza selectia pe nume cand `keep` e dat */
static int load_dir(const char *keep){
    DIR *d = opendir(cwd);
    if(!d){
        set_status("%s: %s", cwd, strerror(errno));
        return -1;
    }
    nents = 0;
    struct dirent *de;
    while((de = readdir(d))){
        if(!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")){
            continue;
        }
        if(!show_hidden && de->d_name[0] == '.'){
            continue;
        }
        if(nents == capents){
            capents = capents ? capents * 2 : 64;
            ents = realloc(ents, (size_t)capents * sizeof(Ent));
            sel = realloc(sel, (size_t)capents);
            if(!ents || !sel){
                closedir(d);
                nents = capents = 0;
                return -1;
            }
        }
        Ent *e = &ents[nents++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        char path[PATH_MAX_Y + 140];
        join(path, sizeof(path), cwd, e->name);
        struct stat st;
        if(stat(path, &st) == 0){
            e->dir = S_ISDIR(st.st_mode);
            e->size = st.st_size;
            e->mode = st.st_mode;
        } else {
            e->dir = de->d_type == DT_DIR;
            e->size = 0;
            e->mode = 0;
        }
    }
    closedir(d);
    cur_sig = dir_sig();
    qsort(ents, (size_t)nents, sizeof(Ent), ent_cmp);
    memset(sel, 0, (size_t)nents);
    cur = anchor = -1;
    if(keep){
        for(int i = 0; i < nents; i++){
            if(!strcmp(ents[i].name, keep)){
                cur = anchor = i;
                break;
            }
        }
    }
    if(scroll > nents){
        scroll = 0;
    }
    dirty = 1;
    return 0;
}

/* Semnatura continutului directorului (nume + dimensiuni), ca sa vedem daca s-a schimbat din afara */
static unsigned long long dir_sig(void){
    unsigned long long h = 1469598103934665603ULL;
    DIR *d = opendir(cwd);
    if(!d){
        return 0;
    }
    struct dirent *de;
    while((de = readdir(d))){
        if(!show_hidden && de->d_name[0] == '.'){
            continue;
        }
        for(const char *p = de->d_name; *p; p++){
            h = (h ^ (unsigned char)*p) * 1099511628211ULL;
        }
        char path[PATH_MAX_Y + 140];
        join(path, sizeof(path), cwd, de->d_name);
        struct stat st;
        if(stat(path, &st) == 0){
            h = (h ^ (unsigned long long)st.st_size ^ ((unsigned long long)st.st_mode << 32)) * 1099511628211ULL;
        }
    }
    closedir(d);
    return h;
}

static void update_title(void){
    char t[64];
    const char *base = strrchr(cwd, '/');
    base = base && base[1] ? base + 1 : "/";
    snprintf(t, sizeof(t), "Fișiere - %s", base);
    yw_window_set_title(win, t);
}

static void navigate(const char *path, const char *keep){
    char real[PATH_MAX_Y];
    snprintf(real, sizeof(real), "%s", path);
    /* normalizeaza: fara "//" si fara "/" final */
    size_t n = strlen(real);
    while(n > 1 && real[n - 1] == '/'){
        real[--n] = 0;
    }
    char old[PATH_MAX_Y];
    snprintf(old, sizeof(old), "%s", cwd);
    snprintf(cwd, sizeof(cwd), "%s", real);
    if(load_dir(keep) < 0){
        snprintf(cwd, sizeof(cwd), "%s", old);
        return;
    }
    scroll = 0;
    status[0] = 0;
    update_title();
}

static void go_up(void){
    if(!strcmp(cwd, "/")){
        return;
    }
    char from[128];
    const char *slash = strrchr(cwd, '/');
    snprintf(from, sizeof(from), "%s", slash + 1);
    char up[PATH_MAX_Y];
    snprintf(up, sizeof(up), "%s", cwd);
    up[slash - cwd] = 0;
    navigate(up[0] ? up : "/", from);
}

/* ---- procese ---- */

static void on_sigchld(int s){
    (void)s;
    child_flag = 1;
}

/* Ruleaza argv, stdout/stdin spre /dev/null, stderr capturat (prima linie in err); 0 = a iesit cu 0 */
static int run_wait(char *const argv[], char *err, size_t cap){
    int p[2];
    if(pipe(p) < 0){
        return -1;
    }
    pid_t pid = fork();
    if(pid < 0){
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if(pid == 0){
        close(yw_fd(conn));
        close(p[0]);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){
            dup2(nul, 0);
            dup2(nul, 1);
        }
        dup2(p[1], 2);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    char buf[256];
    ssize_t r;
    while((r = read(p[0], buf, sizeof(buf))) > 0){
        for(ssize_t i = 0; i < r && n + 1 < cap; i++){
            if(buf[i] == '\n' && n){
                break;
            }
            err[n++] = buf[i];
        }
    }
    err[n] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void spawn_term(const char *title, const char *dir, char *const prog[]){
    pid_t pid = fork();
    if(pid == 0){
        close(yw_fd(conn));
        char *av[16];
        int n = 0;
        av[n++] = "yterm";
        av[n++] = "-t";
        av[n++] = (char *)title;
        if(dir){
            av[n++] = "-d";
            av[n++] = (char *)dir;
        }
        if(prog){
            av[n++] = "-e";
            for(int i = 0; prog[i] && n < 15; i++){
                av[n++] = prog[i];
            }
        }
        av[n] = NULL;
        execv("/bin/yterm", av);
        _exit(127);
    }
}

static int looks_text(const char *path){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return 0;
    }
    unsigned char b[512];
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if(n < 0){
        return 0;
    }
    for(ssize_t i = 0; i < n; i++){
        if(b[i] == 0 || (b[i] < 9) || (b[i] > 13 && b[i] < 32 && b[i] != 27)){
            return 0;
        }
    }
    return 1;
}

static void open_in_editor(const char *path, const char *name){
    char title[96];
    snprintf(title, sizeof(title), "yedit - %s", name);
    char *prog[] = {"/bin/yedit", (char *)path, NULL};
    spawn_term(title, NULL, prog);
}

static void open_path(const Ent *e){
    char path[PATH_MAX_Y + 140];
    join(path, sizeof(path), cwd, e->name);
    if(e->dir){
        navigate(path, NULL);
        return;
    }
    if(e->mode & 0111){
        char *prog[] = {path, NULL};
        spawn_term(e->name, cwd, prog);
        set_status("Pornit: %s", e->name);
    } else if(looks_text(path)){
        open_in_editor(path, e->name);
        set_status("Deschis în editor: %s", e->name);
    } else {
        set_status("%s: nu știu cum să deschid acest fișier", e->name);
    }
}

/* ---- selectie ---- */

static int sel_count(void){
    int n = 0;
    for(int i = 0; i < nents; i++){
        n += sel[i] != 0;
    }
    return n;
}

/* Indicii elementelor vizate: cele selectate, sau cel cu focus */
static int targets(int *out, int max){
    int n = 0;
    for(int i = 0; i < nents && n < max; i++){
        if(sel[i]){
            out[n++] = i;
        }
    }
    if(!n && cur >= 0 && cur < nents){
        out[n++] = cur;
    }
    return n;
}

static void select_only(int i){
    clear_sel();
    if(i >= 0 && i < nents){
        sel[i] = 1;
    }
    cur = anchor = i;
}

static int list_rows(void){
    YgSurface *s = yw_window_surface(win);
    int h = s->height - PATH_H - TOOL_H - STATUS_H - HEAD_H;
    return h / ROW_H > 0 ? h / ROW_H : 1;
}

static void ensure_visible(void){
    int vis = list_rows();
    if(cur < 0){
        return;
    }
    if(cur < scroll){
        scroll = cur;
    }
    if(cur >= scroll + vis){
        scroll = cur - vis + 1;
    }
    int max = nents > vis ? nents - vis : 0;
    if(scroll > max){
        scroll = max;
    }
    if(scroll < 0){
        scroll = 0;
    }
}

static void move_cursor(int to, int extend){
    if(!nents){
        return;
    }
    if(to < 0){
        to = 0;
    }
    if(to >= nents){
        to = nents - 1;
    }
    if(extend){
        if(anchor < 0){
            anchor = cur < 0 ? to : cur;
        }
        clear_sel();
        int a = anchor < to ? anchor : to, b = anchor < to ? to : anchor;
        for(int i = a; i <= b; i++){
            sel[i] = 1;
        }
        cur = to;
    } else {
        select_only(to);
    }
    ensure_visible();
    dirty = 1;
}

/* ---- operatii pe fisiere ---- */

static void clip_clear(void){
    for(int i = 0; i < nclip; i++){
        free(clip[i]);
    }
    nclip = 0;
}

static void do_copy(int cut){
    int idx[64];
    int n = targets(idx, 64);
    if(!n){
        return;
    }
    clip_clear();
    for(int i = 0; i < n; i++){
        char path[PATH_MAX_Y + 140];
        join(path, sizeof(path), cwd, ents[idx[i]].name);
        clip[nclip++] = strdup(path);
    }
    clip_cut = cut;
    set_status("%s %d element%s", cut ? "Tăiat:" : "Copiat:", n, n == 1 ? "" : "e");
}

static int exists(const char *p){
    struct stat st;
    return lstat(p, &st) == 0;
}

/* Nume liber in cwd pentru base: "base", "base (copie)", "base (copie 2)", ... (inainte de extensie) */
static void unique_dest(const char *base, char *out, size_t cap){
    join(out, cap, cwd, base);
    if(!exists(out)){
        return;
    }
    char stem[128], ext[32];
    const char *dot = strrchr(base, '.');
    if(dot && dot != base){
        snprintf(stem, sizeof(stem), "%.*s", (int)(dot - base), base);
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(stem, sizeof(stem), "%s", base);
        ext[0] = 0;
    }
    for(int k = 1; k < 1000; k++){
        char name[300];
        if(k == 1){
            snprintf(name, sizeof(name), "%s (copie)%s", stem, ext);
        } else {
            snprintf(name, sizeof(name), "%s (copie %d)%s", stem, k, ext);
        }
        join(out, cap, cwd, name);
        if(!exists(out)){
            return;
        }
    }
}

static void do_paste(void){
    if(!nclip){
        set_status("Clipboard gol");
        return;
    }
    int ok = 0, fail = 0;
    char err[160] = "";
    char last[128] = "";
    for(int i = 0; i < nclip; i++){
        const char *src = clip[i];
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        size_t sl = strlen(src);
        if(!strncmp(cwd, src, sl) && (cwd[sl] == '/' || cwd[sl] == 0) && strcmp(src, "/") != 0){
            snprintf(err, sizeof(err), "%s: nu se poate lipi în el însuși", base);
            fail++;
            continue;
        }
        char dst[PATH_MAX_Y + 140];
        char dir[PATH_MAX_Y];
        snprintf(dir, sizeof(dir), "%s", src);
        char *sl2 = strrchr(dir, '/');
        if(sl2 == dir){
            dir[1] = 0;
        } else if(sl2){
            *sl2 = 0;
        }
        int same_dir = !strcmp(dir, cwd);
        if(clip_cut && same_dir){
            ok++;
            continue;                           /* mutat in acelasi loc */
        }
        unique_dest(base, dst, sizeof(dst));
        char e[160] = "";
        char *av_cp[] = {"/bin/cp", "-r", (char *)src, dst, NULL};
        char *av_mv[] = {"/bin/mv", (char *)src, dst, NULL};
        if(run_wait(clip_cut ? av_mv : av_cp, e, sizeof(e)) == 0){
            ok++;
            snprintf(last, sizeof(last), "%s", strrchr(dst, '/') + 1);
        } else {
            fail++;
            snprintf(err, sizeof(err), "%s", e[0] ? e : "operația a eșuat");
        }
    }
    if(clip_cut && !fail){
        clip_clear();
    }
    load_dir(last[0] ? last : NULL);
    ensure_visible();
    if(fail){
        set_status("%s", err);
    } else {
        set_status("%s %d element%s", clip_cut ? "Mutat:" : "Lipit:", ok, ok == 1 ? "" : "e");
    }
}

static void do_delete(void){
    int idx[64];
    int n = targets(idx, 64);
    int ok = 0, fail = 0;
    char err[160] = "";
    for(int i = 0; i < n; i++){
        char path[PATH_MAX_Y + 140];
        join(path, sizeof(path), cwd, ents[idx[i]].name);
        if(!strcmp(path, "/")){
            continue;
        }
        char e[160] = "";
        char *av[] = {"/bin/rm", "-rf", path, NULL};
        if(run_wait(av, e, sizeof(e)) == 0 && !exists(path)){
            ok++;
        } else {
            fail++;
            snprintf(err, sizeof(err), "%s: %s", ents[idx[i]].name, e[0] ? e : "nu s-a putut șterge");
        }
    }
    int keep_cur = cur;
    load_dir(NULL);
    if(nents){
        select_only(keep_cur < nents ? keep_cur : nents - 1);
    }
    ensure_visible();
    if(fail){
        set_status("%s", err);
    } else {
        set_status("Șters: %d element%s", ok, ok == 1 ? "" : "e");
    }
}

static void start_prompt(int kind, const char *initial){
    prompt_kind = kind;
    snprintf(prompt_buf, sizeof(prompt_buf), "%s", initial);
    prompt_len = (int)strlen(prompt_buf);
    mode = M_PROMPT;
    dirty = 1;
}

static void finish_prompt(void){
    char text[256];
    snprintf(text, sizeof(text), "%s", prompt_buf);
    mode = M_NORMAL;
    dirty = 1;
    if(!text[0]){
        return;
    }
    if(prompt_kind == A_GOTO){
        struct stat st;
        char full[PATH_MAX_Y + 140];
        if(text[0] == '/'){
            snprintf(full, sizeof(full), "%s", text);
        } else {
            join(full, sizeof(full), cwd, text);
        }
        if(stat(full, &st) == 0 && S_ISDIR(st.st_mode)){
            navigate(full, NULL);
        } else {
            set_status("%s: nu e un director", text);
        }
        return;
    }
    if(strchr(text, '/')){
        set_status("Numele nu poate conține '/'");
        return;
    }
    char path[PATH_MAX_Y + 140];
    join(path, sizeof(path), cwd, text);
    if(prompt_kind == A_RENAME){
        if(cur < 0){
            return;
        }
        char old[PATH_MAX_Y + 140];
        join(old, sizeof(old), cwd, ents[cur].name);
        if(!strcmp(ents[cur].name, text)){
            return;
        }
        if(exists(path)){
            set_status("%s există deja", text);
        } else if(rename(old, path) < 0){
            set_status("Redenumire: %s", strerror(errno));
        } else {
            load_dir(text);
            ensure_visible();
            set_status("Redenumit în %s", text);
        }
    } else if(prompt_kind == A_MKDIR){
        if(mkdir(path, 0755) < 0){
            set_status("Dosar nou: %s", strerror(errno));
        } else {
            load_dir(text);
            ensure_visible();
            set_status("Creat dosarul %s", text);
        }
    } else if(prompt_kind == A_MKFILE){
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if(fd < 0){
            set_status("Fișier nou: %s", strerror(errno));
        } else {
            close(fd);
            load_dir(text);
            ensure_visible();
            set_status("Creat fișierul %s", text);
        }
    }
}

static void do_action(int a){
    int idx[64];
    switch(a){
    case A_UP: go_up(); break;
    case A_HOME: {
        const char *h = getenv("HOME");
        navigate(h && *h ? h : "/", NULL);
        break;
    }
    case A_REFRESH: load_dir(cur >= 0 && cur < nents ? ents[cur].name : NULL); set_status("Reîmprospătat"); break;
    case A_HIDDEN:
        show_hidden = !show_hidden;
        load_dir(NULL);
        set_status(show_hidden ? "Fișiere ascunse: vizibile" : "Fișiere ascunse: ascunse");
        break;
    case A_MKDIR: start_prompt(A_MKDIR, "Dosar nou"); break;
    case A_MKFILE: start_prompt(A_MKFILE, "fisier.txt"); break;
    case A_GOTO: start_prompt(A_GOTO, cwd); break;
    case A_COPY: do_copy(0); break;
    case A_CUT: do_copy(1); break;
    case A_PASTE: do_paste(); break;
    case A_DELETE:
        if(targets(idx, 64)){
            mode = M_CONFIRM;
            dirty = 1;
        }
        break;
    case A_RENAME:
        if(cur >= 0 && cur < nents){
            start_prompt(A_RENAME, ents[cur].name);
        }
        break;
    case A_OPEN:
        if(cur >= 0 && cur < nents){
            open_path(&ents[cur]);
        }
        break;
    case A_EDIT:
        if(cur >= 0 && cur < nents && !ents[cur].dir){
            char path[PATH_MAX_Y + 140];
            join(path, sizeof(path), cwd, ents[cur].name);
            open_in_editor(path, ents[cur].name);
            set_status("Deschis în editor: %s", ents[cur].name);
        } else {
            set_status("Alege un fișier pentru editor");
        }
        break;
    case A_TERM: spawn_term("Terminal", cwd, NULL); set_status("Terminal în %s", cwd); break;
    default: break;
    }
}

/* ---- geometrie ---- */

typedef struct {
    int act;
    const char *label;
} Tool;

static const Tool tools[] = {
    {A_UP, "Sus"}, {A_HOME, "Acasă"}, {A_MKDIR, "Dosar nou"}, {A_COPY, "Copiază"}, {A_CUT, "Taie"}, {A_PASTE, "Lipește"},
    {A_DELETE, "Șterge"}, {A_EDIT, "Editor"}, {A_TERM, "Terminal"},
};
#define NTOOLS ((int)(sizeof(tools) / sizeof(tools[0])))

static YgRect tool_rect(int i){
    int x = 8;
    for(int k = 0; k < i; k++){
        x += yg_text_width(font, 13, tools[k].label) + 26 + 5;
    }
    return yg_rect(x, 6, yg_text_width(font, 13, tools[i].label) + 26, TOOL_H - 12);
}

static YgRect list_rect(void){
    YgSurface *s = yw_window_surface(win);
    return yg_rect(SIDE_W, TOOL_H + PATH_H, s->width - SIDE_W, s->height - TOOL_H - PATH_H - STATUS_H);
}

static YgRect place_rect(int i){
    return yg_rect(6, TOOL_H + PATH_H + 8 + i * 28, SIDE_W - 12, 26);
}

static YgRect row_rect(int vis_row){
    YgRect l = list_rect();
    return yg_rect(l.x, l.y + HEAD_H + vis_row * ROW_H, l.w - 10, ROW_H);
}

#define MENU_ITEMS 13
static const struct { int act; const char *label; } menu_items[MENU_ITEMS] = {
    {A_OPEN, "Deschide"}, {A_EDIT, "Deschide în editor"}, {A_TERM, "Terminal aici"}, {A_NONE, NULL},
    {A_COPY, "Copiază"}, {A_CUT, "Taie"}, {A_PASTE, "Lipește"}, {A_NONE, NULL},
    {A_RENAME, "Redenumește"}, {A_DELETE, "Șterge"}, {A_NONE, NULL}, {A_MKDIR, "Dosar nou"}, {A_MKFILE, "Fișier nou"},
};

static YgRect menu_rect(void){
    YgSurface *s = yw_window_surface(win);
    int h = 8;
    for(int i = 0; i < MENU_ITEMS; i++){
        h += menu_items[i].act ? 24 : 7;
    }
    int x = menu_x, y = menu_y;
    if(x + 190 > s->width){
        x = s->width - 190;
    }
    if(y + h > s->height){
        y = s->height - h;
    }
    return yg_rect(x < 0 ? 0 : x, y < 0 ? 0 : y, 190, h);
}

static YgRect menu_item_rect(int i){
    YgRect m = menu_rect();
    int y = m.y + 4;
    for(int k = 0; k < i; k++){
        y += menu_items[k].act ? 24 : 7;
    }
    return yg_rect(m.x + 3, y, m.w - 6, 24);
}

/* ---- desen ---- */

static void draw_icon(int x, int y, int dir, int selected){
    YgSurface *s = ui.s;
    YgColor c = dir ? (selected ? th->accent_text : th->accent) : (selected ? th->accent_text : th->text_muted);
    if(dir){
        yg_fill_rounded_rect(s, x, y + 3, 8, 5, 2, c);
        yg_fill_rounded_rect(s, x, y + 5, 16, 11, 2, c);
    } else {
        yg_draw_rounded_rect(s, x + 2, y + 1, 12, 15, 2, 1, c);
        yg_fill_rect(s, x + 5, y + 6, 6, 1, c);
        yg_fill_rect(s, x + 5, y + 9, 6, 1, c);
        yg_fill_rect(s, x + 5, y + 12, 4, 1, c);
    }
}

static void draw_all(void){
    YgSurface *s = yw_window_surface(win);
    ui.s = s;
    int W = s->width, H = s->height;
    yg_clear(s, th->surface);
    /* bara de unelte */
    yg_fill_rect(s, 0, 0, W, TOOL_H, th->surface_alt);
    yg_fill_rect(s, 0, TOOL_H - 1, W, 1, th->border);
    for(int i = 0; i < NTOOLS; i++){
        int en = 1;
        if(tools[i].act == A_PASTE){
            en = nclip > 0;
        } else if(tools[i].act == A_COPY || tools[i].act == A_CUT || tools[i].act == A_DELETE){
            en = cur >= 0 || sel_count() > 0;
        } else if(tools[i].act == A_UP){
            en = strcmp(cwd, "/") != 0;
        }
        yui_button(&ui, tool_rect(i), tools[i].label, hover_kind == 1 && hover_idx == i && en ? 1 : 0, en);
    }
    /* cale */
    yg_fill_rect(s, 0, TOOL_H, W, PATH_H, th->surface);
    {
        char p[PATH_MAX_Y];
        yui_fit(&ui, 13, cwd, W - 24, p, sizeof(p));
        yui_text(&ui, 13, yg_rect(4, TOOL_H, W - 8, PATH_H), p, th->text_muted, YUI_LEFT, 0);
    }
    /* locuri */
    yg_fill_rect(s, 0, TOOL_H + PATH_H, SIDE_W, H - TOOL_H - PATH_H - STATUS_H, th->surface_alt);
    yg_fill_rect(s, SIDE_W - 1, TOOL_H + PATH_H, 1, H - TOOL_H - PATH_H - STATUS_H, th->border);
    for(int i = 0; i < nplaces; i++){
        YgRect r = place_rect(i);
        int here = !strcmp(places[i], cwd);
        if(here){
            yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, th->accent);
        } else if(hover_kind == 2 && hover_idx == i){
            yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, yg_mix(th->surface_alt, th->text, 28));
        }
        yui_text(&ui, 13, r, place_names[i], here ? th->accent_text : th->text, YUI_LEFT, here);
    }
    /* lista */
    YgRect l = list_rect();
    yg_fill_rect(s, l.x, l.y, l.w, HEAD_H, th->surface_alt);
    yui_text(&ui, 12, yg_rect(l.x + 28, l.y, l.w - 280, HEAD_H), "Nume", th->text_muted, YUI_LEFT, 1);
    yui_text(&ui, 12, yg_rect(l.x + l.w - 250, l.y, 80, HEAD_H), "Dimensiune", th->text_muted, YUI_RIGHT, 1);
    yui_text(&ui, 12, yg_rect(l.x + l.w - 160, l.y, 140, HEAD_H), "Drepturi", th->text_muted, YUI_LEFT, 1);
    int vis = list_rows();
    for(int r = 0; r < vis && scroll + r < nents; r++){
        int i = scroll + r;
        YgRect rr = row_rect(r);
        int selected = sel[i];
        if(selected){
            yg_fill_rect(s, rr.x, rr.y, rr.w, rr.h, th->accent);
        } else if(hover_kind == 3 && hover_idx == i){
            yg_fill_rect(s, rr.x, rr.y, rr.w, rr.h, yg_mix(th->surface, th->text, 22));
        }
        if(i == cur && !selected){
            yg_draw_rect(s, rr.x, rr.y, rr.w, rr.h, 1, th->accent);
        }
        YgColor fg = selected ? th->accent_text : th->text;
        draw_icon(rr.x + 6, rr.y + 4, ents[i].dir, selected);
        char nm[200];
        yui_fit(&ui, 13, ents[i].name, rr.w - 300, nm, sizeof(nm));
        yui_text(&ui, 13, yg_rect(rr.x + 28, rr.y, rr.w - 280, rr.h), nm, fg, YUI_LEFT, ents[i].dir);
        if(!ents[i].dir){
            char sz[32];
            fmt_size(ents[i].size, sz, sizeof(sz));
            yui_text(&ui, 12, yg_rect(rr.x + rr.w - 250, rr.y, 80, rr.h), sz, selected ? th->accent_text : th->text_muted, YUI_RIGHT, 0);
        }
        char md[16];
        fmt_mode(ents[i].mode, md);
        yui_text(&ui, 12, yg_rect(rr.x + rr.w - 160, rr.y, 140, rr.h), md, selected ? th->accent_text : th->text_muted, YUI_LEFT, 0);
    }
    if(!nents){
        yui_text(&ui, 13, yg_rect(l.x, l.y + HEAD_H + 10, l.w, 24), "(gol)", th->text_muted, YUI_CENTER, 0);
    }
    yui_scrollbar(&ui, yg_rect(l.x + l.w - 9, l.y + HEAD_H, 8, vis * ROW_H), nents, vis, scroll);
    /* stare */
    yg_fill_rect(s, 0, H - STATUS_H, W, STATUS_H, th->surface_alt);
    yg_fill_rect(s, 0, H - STATUS_H, W, 1, th->border);
    {
        char st[256];
        if(status[0]){
            snprintf(st, sizeof(st), "%s", status);
        } else if(sel_count() > 1){
            snprintf(st, sizeof(st), "%d selectate din %d", sel_count(), nents);
        } else if(cur >= 0 && cur < nents){
            char sz[32];
            fmt_size(ents[cur].size, sz, sizeof(sz));
            snprintf(st, sizeof(st), "%s - %s", ents[cur].name, ents[cur].dir ? "director" : sz);
        } else {
            snprintf(st, sizeof(st), "%d elemente", nents);
        }
        yui_text(&ui, 12, yg_rect(4, H - STATUS_H + 1, W - 8, STATUS_H - 1), st, th->text_muted, YUI_LEFT, 0);
    }
    /* meniu / dialoguri */
    if(mode == M_MENU){
        YgRect m = menu_rect();
        yg_fill_rounded_rect(s, m.x + 2, m.y + 3, m.w, m.h, 6, YG_ARGB(90, 0, 0, 0));
        yg_fill_rounded_rect(s, m.x, m.y, m.w, m.h, 6, th->surface_alt);
        yg_draw_rounded_rect(s, m.x, m.y, m.w, m.h, 6, 1, th->border);
        for(int i = 0; i < MENU_ITEMS; i++){
            YgRect r = menu_item_rect(i);
            if(!menu_items[i].act){
                yg_fill_rect(s, r.x + 6, r.y + 3, r.w - 12, 1, th->border);
                continue;
            }
            int hov = hover_kind == 4 && hover_idx == i;
            if(hov){
                yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 4, th->accent);
            }
            int en = menu_items[i].act != A_PASTE || nclip > 0;
            yui_text(&ui, 13, r, menu_items[i].label, hov ? th->accent_text : en ? th->text : th->text_muted, YUI_LEFT, 0);
        }
    } else if(mode == M_PROMPT){
        const char *title = prompt_kind == A_RENAME ? "Redenumește" : prompt_kind == A_MKDIR ? "Dosar nou" :
                            prompt_kind == A_MKFILE ? "Fișier nou" : "Mergi la";
        YgRect d = yui_dialog(&ui, 360, 110, title);
        yui_field(&ui, yg_rect(d.x + 12, d.y + 40, d.w - 24, 26), prompt_buf, prompt_len, 1);
        yui_button(&ui, yg_rect(d.x + d.w - 180, d.y + 76, 80, 26), "Anulează", 0, 1);
        yui_button(&ui, yg_rect(d.x + d.w - 92, d.y + 76, 80, 26), "OK", 2, 1);
    } else if(mode == M_CONFIRM){
        int idx[64];
        int n = targets(idx, 64);
        char msg[200];
        if(n == 1){
            snprintf(msg, sizeof(msg), "Ștergi „%s”?", ents[idx[0]].name);
        } else {
            snprintf(msg, sizeof(msg), "Ștergi %d elemente?", n);
        }
        YgRect d = yui_dialog(&ui, 360, 110, "Confirmare");
        char fit[200];
        yui_fit(&ui, 13, msg, d.w - 24, fit, sizeof(fit));
        yui_text(&ui, 13, yg_rect(d.x + 12, d.y + 40, d.w - 24, 24), fit, th->text, YUI_LEFT, 0);
        yui_button(&ui, yg_rect(d.x + d.w - 180, d.y + 76, 80, 26), "Nu", 0, 1);
        yui_button(&ui, yg_rect(d.x + d.w - 92, d.y + 76, 80, 26), "Șterge", 2, 1);
    }
    yw_window_damage_all(win);
}

/* ---- evenimente ---- */

static void open_menu(int x, int y){
    mode = M_MENU;
    menu_x = x;
    menu_y = y;
    hover_kind = 0;
    dirty = 1;
}

static int row_at(int x, int y){
    YgRect l = list_rect();
    if(x < l.x || x >= l.x + l.w - 10 || y < l.y + HEAD_H){
        return -1;
    }
    int r = (y - l.y - HEAD_H) / ROW_H;
    int i = scroll + r;
    return r < list_rows() && i < nents ? i : -1;
}

static void hit_hover(int x, int y){
    int k = 0, idx = -1;
    if(mode == M_MENU){
        for(int i = 0; i < MENU_ITEMS; i++){
            if(menu_items[i].act && yui_in(menu_item_rect(i), x, y)){
                k = 4;
                idx = i;
            }
        }
    } else if(mode == M_NORMAL){
        for(int i = 0; i < NTOOLS; i++){
            if(yui_in(tool_rect(i), x, y)){
                k = 1;
                idx = i;
            }
        }
        for(int i = 0; i < nplaces; i++){
            if(yui_in(place_rect(i), x, y)){
                k = 2;
                idx = i;
            }
        }
        int r = row_at(x, y);
        if(r >= 0){
            k = 3;
            idx = r;
        }
    }
    if(k != hover_kind || idx != hover_idx){
        hover_kind = k;
        hover_idx = idx;
        dirty = 1;
    }
}

static void on_mouse(const YwEvent *ev){
    int x = ev->x, y = ev->y;
    if(ev->kind == YW_MOUSE_MOVE){
        hit_hover(x, y);
        return;
    }
    if(ev->kind == YW_MOUSE_WHEEL){
        if(mode == M_NORMAL){
            scroll -= ev->wheel > 0 ? 3 : -3;
            int vis = list_rows();
            int max = nents > vis ? nents - vis : 0;
            if(scroll > max){
                scroll = max;
            }
            if(scroll < 0){
                scroll = 0;
            }
            dirty = 1;
        }
        return;
    }
    if(ev->kind != YW_MOUSE_PRESS){
        return;
    }
    int left = (ev->buttons & YK_BTN_LEFT) != 0, right = (ev->buttons & YK_BTN_RIGHT) != 0;
    if(mode == M_MENU){
        mode = M_NORMAL;
        dirty = 1;
        if(left){
            for(int i = 0; i < MENU_ITEMS; i++){
                if(menu_items[i].act && yui_in(menu_item_rect(i), x, y)){
                    if(menu_items[i].act != A_PASTE || nclip){
                        do_action(menu_items[i].act);
                    }
                    break;
                }
            }
        }
        return;
    }
    if(mode == M_PROMPT || mode == M_CONFIRM){
        YgSurface *s = yw_window_surface(win);
        YgRect d = yg_rect((s->width - 360) / 2, (s->height - 110) / 2, 360, 110);
        YgRect no = yg_rect(d.x + d.w - 180, d.y + 76, 80, 26), yes = yg_rect(d.x + d.w - 92, d.y + 76, 80, 26);
        if(left && yui_in(yes, x, y)){
            if(mode == M_PROMPT){
                finish_prompt();
            } else {
                mode = M_NORMAL;
                do_delete();
            }
        } else if(left && yui_in(no, x, y)){
            mode = M_NORMAL;
            dirty = 1;
        }
        return;
    }
    if(!left && !right){
        return;
    }
    for(int i = 0; i < NTOOLS && left; i++){
        if(yui_in(tool_rect(i), x, y)){
            do_action(tools[i].act);
            return;
        }
    }
    for(int i = 0; i < nplaces && left; i++){
        if(yui_in(place_rect(i), x, y)){
            navigate(places[i], NULL);
            return;
        }
    }
    int i = row_at(x, y);
    if(i < 0){
        if(left && x >= SIDE_W && y > TOOL_H + PATH_H){
            clear_sel();
            cur = anchor = -1;
            dirty = 1;
        }
        if(right){
            open_menu(x, y);
        }
        return;
    }
    if(right){
        if(!sel[i]){
            select_only(i);
        }
        cur = i;
        open_menu(x, y);
        return;
    }
    if(ev->modifiers & YK_MOD_CTRL){
        sel[i] = !sel[i];
        cur = anchor = i;
    } else if(ev->modifiers & YK_MOD_SHIFT){
        move_cursor(i, 1);
    } else {
        long long t = now_ms();
        if(i == last_click_idx && t - last_click_ms < 500){
            select_only(i);
            last_click_idx = -1;
            open_path(&ents[i]);
            dirty = 1;
            return;
        }
        last_click_idx = i;
        last_click_ms = t;
        select_only(i);
    }
    dirty = 1;
}

static void on_key(const YwEvent *ev){
    if(!ev->pressed){
        return;
    }
    int k = ev->keycode, ctrl = (ev->modifiers & YK_MOD_CTRL) != 0, shift = (ev->modifiers & YK_MOD_SHIFT) != 0;
    int a = ev->ascii;
    if(mode == M_MENU){
        mode = M_NORMAL;
        dirty = 1;
        return;
    }
    if(mode == M_PROMPT){
        if(k == K_ESC){
            mode = M_NORMAL;
        } else if(k == K_ENTER){
            finish_prompt();
        } else if(k == K_BS){
            if(prompt_len > 0){
                prompt_buf[--prompt_len] = 0;
            }
        } else if(a >= 32 && a < 127 && !ctrl && prompt_len < (int)sizeof(prompt_buf) - 2){
            prompt_buf[prompt_len++] = (char)a;
            prompt_buf[prompt_len] = 0;
        }
        dirty = 1;
        return;
    }
    if(mode == M_CONFIRM){
        if(k == K_ENTER || a == 'y' || a == 'Y' || a == 'd' || a == 'D'){
            mode = M_NORMAL;
            do_delete();
        } else if(k == K_ESC || a == 'n' || a == 'N'){
            mode = M_NORMAL;
            dirty = 1;
        }
        return;
    }
    status[0] = 0;
    dirty = 1;
    if(ctrl){
        switch(a | 0x20){
        case 'c': do_action(A_COPY); return;
        case 'x': do_action(A_CUT); return;
        case 'v': do_action(A_PASTE); return;
        case 'n': do_action(shift ? A_MKFILE : A_MKDIR); return;
        case 'l': do_action(A_GOTO); return;
        case 'h': do_action(A_HIDDEN); return;
        case 'r': do_action(A_REFRESH); return;
        case 'a':
            for(int i = 0; i < nents; i++){
                sel[i] = 1;
            }
            if(cur < 0 && nents){
                cur = 0;
            }
            return;
        default: break;
        }
    }
    switch(k){
    case K_UP: move_cursor(cur < 0 ? nents - 1 : cur - 1, shift); return;
    case K_DOWN: move_cursor(cur < 0 ? 0 : cur + 1, shift); return;
    case K_PGUP: move_cursor((cur < 0 ? 0 : cur) - list_rows() + 1, shift); return;
    case K_PGDN: move_cursor((cur < 0 ? 0 : cur) + list_rows() - 1, shift); return;
    case K_HOME: move_cursor(0, shift); return;
    case K_END: move_cursor(nents - 1, shift); return;
    case K_ENTER: do_action(A_OPEN); return;
    case K_BS: do_action(A_UP); return;
    case K_DEL: do_action(A_DELETE); return;
    case K_F2: do_action(A_RENAME); return;
    case K_F4: do_action(A_EDIT); return;
    case K_F5: do_action(A_REFRESH); return;
    case K_ESC: clear_sel(); dirty = 1; return;
    default: break;
    }
    if(a > 32 && a < 127 && !ctrl){
        /* salt la primul nume care incepe cu literele tastate repede */
        long long t = now_ms();
        size_t n = strlen(typed);
        if(t - typed_ms > 900 || n + 1 >= sizeof(typed)){
            n = 0;
        }
        typed[n++] = (char)a;
        typed[n] = 0;
        typed_ms = t;
        for(int i = 0; i < nents; i++){
            if(!strncasecmp(ents[i].name, typed, n)){
                select_only(i);
                ensure_visible();
                break;
            }
        }
    }
}

static void make_places(void){
    const char *home = getenv("HOME");
    const struct { const char *name, *path; } want[] = {
        {"Acasă", home && *home ? home : "/root"}, {"Rădăcină", "/"}, {"/tmp", "/tmp"}, {"/etc", "/etc"}, {"/media", "/media"},
        {"/bin", "/bin"},
    };
    for(int i = 0; i < 6; i++){
        struct stat st;
        if(stat(want[i].path, &st) == 0 && S_ISDIR(st.st_mode)){
            snprintf(places[nplaces], sizeof(places[0]), "%s", want[i].path);
            place_names[nplaces] = want[i].name;
            nplaces++;
        }
    }
}

static int usage(void){
    printf("uz: yfiles [-s socket] [-t dark|light] [director]\n");
    return 2;
}

int main(int argc, char **argv){
    const char *sock = NULL, *theme = NULL, *start = NULL;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            usage();
            return 0;
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            sock = argv[++i];
        } else if(!strcmp(argv[i], "-t") && i + 1 < argc){
            theme = argv[++i];
        } else if(argv[i][0] != '-' && !start){
            start = argv[i];
        } else {
            return usage();
        }
    }
    for(int i = 0; i < 50 && !conn; i++){
        conn = yw_connect(sock);
        if(!conn){
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
    }
    if(!conn){
        perror("yfiles: conectare");
        return 1;
    }
    th = theme ? yg_theme_by_name(theme) : NULL;
    if(!th){
        int32_t q[6];
        th = yw_query(conn, YW_Q_THEME, 0, 0, q) == 0 && q[0] ? &yg_theme_light : &yg_theme_dark;
    }
    font = yg_font_load(YG_FONT_REGULAR);
    bold = yg_font_load(YG_FONT_BOLD);
    if(!font){
        fprintf(stderr, "yfiles: fara font\n");
        return 1;
    }
    ui.f = font;
    ui.b = bold;
    ui.t = th;
    win = yw_window_new(conn, 760, 460, "Fișiere");
    if(!win){
        perror("yfiles: fereastra");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    sigaction(SIGCHLD, &sa, NULL);
    make_places();
    const char *home = getenv("HOME");
    char first[PATH_MAX_Y];
    snprintf(first, sizeof(first), "%s", start ? start : home && *home ? home : "/");
    if(first[0] != '/'){
        char tmp[PATH_MAX_Y];
        if(getcwd(tmp, sizeof(tmp))){
            snprintf(first, sizeof(first), "%s/%s", tmp, start);
        }
    }
    navigate(first, NULL);
    if(!ents){
        navigate("/", NULL);
    }
    long long last_scan = now_ms();
    for(;;){
        ui.t = th;
        if(dirty && !yw_pending(conn)){
            dirty = 0;
            draw_all();
        }
        if(child_flag){
            child_flag = 0;
            while(waitpid(-1, NULL, WNOHANG) > 0){
            }
        }
        struct pollfd pf = {yw_fd(conn), POLLIN, 0};
        int got = yw_pending(conn) > 0;
        if(!got){
            int r = poll(&pf, 1, 2000);
            if(r < 0 && errno != EINTR){
                break;
            }
            got = r > 0;
            if(!got && mode == M_NORMAL && now_ms() - last_scan >= 2000){
                /* directorul se poate schimba din afara (terminal, alte aplicatii) */
                last_scan = now_ms();
                unsigned long long sg = dir_sig();
                if(sg != cur_sig){
                    char keep[128] = "";
                    if(cur >= 0 && cur < nents){
                        snprintf(keep, sizeof(keep), "%s", ents[cur].name);
                    }
                    int svscroll = scroll;
                    if(load_dir(keep[0] ? keep : NULL) == 0){
                        scroll = svscroll;
                        ensure_visible();
                    }
                }
            }
        }
        if(got){
            YwEvent ev;
            int r = yw_next_event(conn, &ev, 0);
            if(r < 0){
                break;
            }
            if(r == 0){
                continue;
            }
            if(ev.type == YW_EV_CLOSE){
                break;
            } else if(ev.type == YW_EV_KEY){
                on_key(&ev);
            } else if(ev.type == YW_EV_MOUSE){
                on_mouse(&ev);
            } else if(ev.type == YW_EV_LEAVE){
                if(hover_kind){
                    hover_kind = 0;
                    dirty = 1;
                }
            } else if(ev.type == YW_EV_RESIZE){
                ensure_visible();
                dirty = 1;
            } else if(ev.type == YW_EV_THEME){
                th = ev.light ? &yg_theme_light : &yg_theme_dark;
                dirty = 1;
            }
        }
    }
    clip_clear();
    yw_window_destroy(win);
    yw_disconnect(conn);
    return 0;
}
