/* yterm (T10.4): terminal grafic minim pentru ycomp. Nu exista pty in YokaiOS, deci nu e un emulator VT: scrii o linie, se
 * executa cu `ysh -c` (stdin = /dev/null, iesirea si erorile intra in fereastra), cu Enter. Interne: cd, clear, exit.
 * Sus / Jos = istoric, Ctrl+C opreste comanda curenta, Ctrl+L sterge, rotita / PgUp / PgDn deruleaza.
 *   yterm [-s socket] [-t titlu] [-w latime] [-h inaltime] [-x comanda]  (-x: ruleaza comanda la pornire)
 * Programele interactive (editoare, ysh fara -c) nu merg aici. */
#include "ywin.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/yinput.h>

#define NLINES   500
#define LINE_MAX_CH 200
#define HIST_N   40
#define FONT_PX  14
#define LINE_H   18
#define K_PGUP   74
#define K_PGDN   79
#define K_LEFT   75
#define K_RIGHT  76
#define K_HOME   72
#define K_END    77
#define K_DEL    81

static char lines[NLINES][LINE_MAX_CH + 1];
static int nlines = 1;          /* liniile logice; ultima e cea in lucru (fara \n inca) */
static int head;                /* indicele fizic al primei linii */
static char input[LINE_MAX_CH + 1];
static int inlen, incur;
static char hist[HIST_N][LINE_MAX_CH + 1];
static int nhist, hpos;
static int scroll_up;           /* randuri vizuale ascunse sub fereastra */
static int wc[256];             /* latimea fiecarui caracter */

static YwConn *conn;
static YwWindow *win;
static YgFont *font;
static const YgTheme *th = &yg_theme_dark;
static pid_t child;
static int child_fd = -1;
static int running = 1;
static int dirty;

static char *line_at(int i){
    return lines[(head + i) % NLINES];
}

static void new_line(void){
    if(nlines == NLINES){
        head = (head + 1) % NLINES;
        nlines--;
    }
    nlines++;
    line_at(nlines - 1)[0] = 0;
}

static void put_char(unsigned char c){
    char *l = line_at(nlines - 1);
    size_t n = strlen(l);
    if(c == '\n'){
        new_line();
    } else if(c == '\t'){
        for(int i = 0; i < 4 && n + 1 < LINE_MAX_CH; i++, n++){
            l[n] = ' ';
            l[n + 1] = 0;
        }
    } else if(c >= 32 || c >= 0x80){
        if(n + 1 >= LINE_MAX_CH){
            new_line();
            l = line_at(nlines - 1);
            n = 0;
        }
        l[n] = (char)c;
        l[n + 1] = 0;
    }
}

static void put_str(const char *s){
    for(; *s; s++){
        put_char((unsigned char)*s);
    }
}

static int cwidth(unsigned char c){
    return c < 128 ? wc[c] : wc['m'];
}

/* Cate caractere din s[from..] incap pe un rand de maxw pixeli (cel putin unul) */
static int fit(const char *s, int from, int maxw){
    int w = 0, n = 0;
    while(s[from + n]){
        int cw = cwidth((unsigned char)s[from + n]);
        if(w + cw > maxw && n > 0){
            break;
        }
        w += cw;
        n++;
    }
    return n;
}

static void prompt_str(char *out, size_t cap){
    char cwd[96];
    if(!getcwd(cwd, sizeof(cwd))){
        strcpy(cwd, "?");
    }
    snprintf(out, cap, "%s$ ", cwd);
}

/* Un rand vizual: textul, culoarea, daca poarta cursorul si unde */
typedef struct {
    char text[LINE_MAX_CH + 40];
    YgColor col;
    int caret;              /* pozitia cursorului in text sau -1 */
} VRow;

static VRow rows[1500];
static int nrows;

static void add_row(const char *s, int len, YgColor col, int caret){
    if(nrows >= (int)(sizeof(rows) / sizeof(rows[0]))){
        memmove(rows, rows + 1, sizeof(rows) - sizeof(rows[0]));
        nrows--;
    }
    VRow *r = &rows[nrows++];
    if(len > (int)sizeof(r->text) - 1){
        len = (int)sizeof(r->text) - 1;
    }
    memcpy(r->text, s, (size_t)len);
    r->text[len] = 0;
    r->col = col;
    r->caret = caret;
}

/* Imparte textul pe randuri de maxw pixeli */
static void wrap(const char *s, int maxw, YgColor col, int caret_pos){
    int from = 0, len = (int)strlen(s);
    if(!len){
        add_row("", 0, col, caret_pos == 0 ? 0 : -1);
        return;
    }
    while(from < len){
        int n = fit(s, from, maxw);
        int c = -1;
        if(caret_pos >= from && (caret_pos < from + n || (caret_pos == len && from + n == len))){
            c = caret_pos - from;
        }
        add_row(s + from, n, col, c);
        from += n;
    }
}

static void render(void){
    YgSurface *s = yw_window_surface(win);
    int W = s->width, H = s->height;
    int maxw = W - 12;
    yg_clear(s, th->background);

    /* toate liniile se impart pe randuri vizuale (cele mai vechi cad cand randurile se termina) */
    nrows = 0;
    int start = 0;
    for(int i = start; i < nlines - 1; i++){
        wrap(line_at(i), maxw, th->text, -1);
    }
    /* ultima linie: iesire partiala (daca ruleaza ceva) sau prompt + intrare */
    if(child > 0){
        wrap(line_at(nlines - 1), maxw, th->text, -1);
    } else {
        char all[LINE_MAX_CH * 2 + 120];
        char pr[110];
        prompt_str(pr, sizeof(pr));
        const char *partial = line_at(nlines - 1);
        snprintf(all, sizeof(all), "%s%s%s", partial, pr, input);
        wrap(all, maxw, th->text, (int)(strlen(partial) + strlen(pr)) + incur);
    }
    int vis = H / LINE_H;
    int total = nrows;
    int max_scroll = total > vis ? total - vis : 0;
    if(scroll_up > max_scroll){
        scroll_up = max_scroll;
    }
    int bottom = total - scroll_up;            /* exclusiv */
    int top = bottom - vis;
    if(top < 0){
        top = 0;
    }
    int asc, desc, gap;
    yg_font_vmetrics(font, FONT_PX, &asc, &desc, &gap);
    for(int r = top; r < bottom; r++){
        int y = (r - top) * LINE_H;
        int base = y + (LINE_H + asc + desc) / 2;
        if(rows[r].text[0]){
            yg_draw_text(s, font, FONT_PX, 6, base, rows[r].text, rows[r].col);
        }
        if(rows[r].caret >= 0 && child <= 0){
            char tmp[LINE_MAX_CH + 40];
            snprintf(tmp, sizeof(tmp), "%.*s", rows[r].caret, rows[r].text);
            int cx = 6 + (int)strlen(tmp) * 0;
            int w = 0;
            for(const char *p = tmp; *p; p++){
                w += cwidth((unsigned char)*p);
            }
            cx += w;
            yg_fill_rect(s, cx, y + 2, 2, LINE_H - 4, th->accent);
        }
    }
    if(scroll_up){
        yg_fill_rect(s, W - 4, 0, 3, H, yg_mix(th->background, th->text_muted, 80));
    }
    yw_window_damage_all(win);
}

static void run_command(const char *cmd){
    char pr[110];
    prompt_str(pr, sizeof(pr));
    put_str(pr);
    put_str(cmd);
    put_char('\n');
    while(*cmd == ' ' || *cmd == '\t'){
        cmd++;
    }
    if(!*cmd){
        return;
    }
    if(!strcmp(cmd, "exit")){
        running = 0;
        return;
    }
    if(!strcmp(cmd, "clear")){
        nlines = 1;
        head = 0;
        lines[0][0] = 0;
        return;
    }
    if(!strncmp(cmd, "cd", 2) && (cmd[2] == 0 || cmd[2] == ' ')){
        const char *dir = cmd[2] ? cmd + 3 : getenv("HOME");
        while(dir && *dir == ' '){
            dir++;
        }
        if(!dir || !*dir){
            dir = "/";
        }
        if(chdir(dir) < 0){
            put_str("cd: ");
            put_str(dir);
            put_str(": ");
            put_str(strerror(errno));
            put_char('\n');
        }
        return;
    }
    int p[2];
    if(pipe(p) < 0){
        put_str("yterm: pipe\n");
        return;
    }
    pid_t pid = fork();
    if(pid < 0){
        close(p[0]);
        close(p[1]);
        put_str("yterm: fork\n");
        return;
    }
    if(pid == 0){
        close(yw_fd(conn));
        close(p[0]);
        int nul = open("/dev/null", O_RDONLY);
        if(nul >= 0){
            dup2(nul, 0);
        }
        dup2(p[1], 1);
        dup2(p[1], 2);
        if(p[1] > 2){
            close(p[1]);
        }
        char *av[] = {"ysh", "-c", (char *)cmd, NULL};
        execv("/bin/ysh", av);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    child = pid;
    child_fd = p[0];
}

static void child_done(void){
    int st = 0;
    if(child_fd >= 0){
        close(child_fd);
        child_fd = -1;
    }
    if(child > 0){
        waitpid(child, &st, 0);
        child = 0;
    }
    char *l = line_at(nlines - 1);
    if(l[0]){
        put_char('\n');             /* iesire fara \n final: promptul porneste pe rand nou */
    }
    if(st != 0 && ((st >> 8) & 0xFF) != 0){
        char b[40];
        snprintf(b, sizeof(b), "[iesit cu %d]\n", (st >> 8) & 0xFF);
        put_str(b);
    }
}

static void read_child(void){
    char buf[512];
    for(;;){
        ssize_t n = read(child_fd, buf, sizeof(buf));
        if(n > 0){
            for(ssize_t i = 0; i < n; i++){
                put_char((unsigned char)buf[i]);
            }
            scroll_up = 0;
            continue;
        }
        if(n == 0){
            child_done();
        } else if(errno != EAGAIN && errno != EINTR){
            child_done();
        }
        return;
    }
}

static void hist_add(const char *c){
    if(!*c || (nhist && !strcmp(hist[nhist - 1], c))){
        hpos = nhist;
        return;
    }
    if(nhist == HIST_N){
        memmove(hist[0], hist[1], sizeof(hist[0]) * (HIST_N - 1));
        nhist--;
    }
    snprintf(hist[nhist++], sizeof(hist[0]), "%s", c);
    hpos = nhist;
}

static void set_input(const char *s){
    snprintf(input, sizeof(input), "%s", s);
    inlen = incur = (int)strlen(input);
}

static void key(const YwEvent *ev){
    if(!ev->pressed){
        return;
    }
    int ctrl = (ev->modifiers & YK_MOD_CTRL) != 0;
    if(ctrl && (ev->ascii == 'c' || ev->ascii == 'C')){
        if(child > 0){
            kill(child, SIGINT);
            kill(child, SIGTERM);
        } else {
            input[0] = 0;
            inlen = incur = 0;
        }
        dirty = 1;
        return;
    }
    if(ctrl && (ev->ascii == 'l' || ev->ascii == 'L')){
        nlines = 1;
        head = 0;
        lines[0][0] = 0;
        dirty = 1;
        return;
    }
    if(ev->keycode == K_PGUP){
        scroll_up += 8;
        dirty = 1;
        return;
    }
    if(ev->keycode == K_PGDN){
        scroll_up = scroll_up > 8 ? scroll_up - 8 : 0;
        dirty = 1;
        return;
    }
    if(child > 0){
        return;                     /* cat ruleaza o comanda, tastele nu au unde sa mearga (stdin = /dev/null) */
    }
    scroll_up = 0;
    switch(ev->keycode){
    case YK_KEY_ENTER: {
        char cmd[LINE_MAX_CH + 1];
        snprintf(cmd, sizeof(cmd), "%s", input);
        input[0] = 0;
        inlen = incur = 0;
        hist_add(cmd);
        run_command(cmd);
        break;
    }
    case YK_KEY_BACKSPACE:
        if(incur > 0){
            memmove(input + incur - 1, input + incur, (size_t)(inlen - incur + 1));
            incur--;
            inlen--;
        }
        break;
    case K_DEL:
        if(incur < inlen){
            memmove(input + incur, input + incur + 1, (size_t)(inlen - incur));
            inlen--;
        }
        break;
    case K_LEFT:
        if(incur > 0) incur--;
        break;
    case K_RIGHT:
        if(incur < inlen) incur++;
        break;
    case K_HOME:
        incur = 0;
        break;
    case K_END:
        incur = inlen;
        break;
    case YK_KEY_UP:
        if(hpos > 0){
            hpos--;
            set_input(hist[hpos]);
        }
        break;
    case YK_KEY_DOWN:
        if(hpos < nhist){
            hpos++;
            set_input(hpos < nhist ? hist[hpos] : "");
        }
        break;
    default:
        if(ev->ascii >= 32 && ev->ascii < 127 && !ctrl && inlen < LINE_MAX_CH - 1){
            memmove(input + incur + 1, input + incur, (size_t)(inlen - incur + 1));
            input[incur++] = (char)ev->ascii;
            inlen++;
        }
        break;
    }
    dirty = 1;
}

static int usage(void){
    printf("uz: yterm [-s socket] [-t titlu] [-w latime] [-h inaltime] [-x comanda]\n");
    return 2;
}

int main(int argc, char **argv){
    const char *sock = NULL, *title = "Terminal", *first = NULL;
    int ww = 640, wh = 400;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            usage();
            return 0;
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            sock = argv[++i];
        } else if(!strcmp(argv[i], "-t") && i + 1 < argc){
            title = argv[++i];
        } else if(!strcmp(argv[i], "-w") && i + 1 < argc){
            ww = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-h") && i + 1 < argc){
            wh = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-x") && i + 1 < argc){
            first = argv[++i];
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
        perror("yterm: conectare");
        return 1;
    }
    {
        int32_t q[6];
        if(yw_query(conn, YW_Q_THEME, 0, 0, q) == 0 && q[0]){
            th = &yg_theme_light;
        }
    }
    font = yg_font_load(YG_FONT_REGULAR);
    if(!font){
        fprintf(stderr, "yterm: fara font\n");
        return 1;
    }
    for(int c = 0; c < 256; c++){
        char s[2] = {(char)(c ? c : 1), 0};
        wc[c] = c < 32 ? 0 : yg_text_width(font, FONT_PX, s);
    }
    win = yw_window_new(conn, ww, wh, title);
    if(!win){
        perror("yterm: fereastra");
        return 1;
    }
    lines[0][0] = 0;
    put_str("YokaiOS terminal. Comenzi: orice program din /bin; cd, clear, exit.\n");
    if(first){
        run_command(first);
    }
    render();

    while(running){
        struct pollfd pf[2];
        int n = 0;
        pf[n].fd = yw_fd(conn);
        pf[n].events = POLLIN;
        n++;
        if(child_fd >= 0){
            pf[n].fd = child_fd;
            pf[n].events = POLLIN;
            n++;
        }
        YwEvent ev;
        int got = 0;
        if(yw_pending(conn) > 0){
            got = 1;
        } else {
            int r = poll(pf, (nfds_t)n, 1000);
            if(r < 0 && errno != EINTR){
                break;
            }
            if(r > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))){
                got = 1;
            }
            if(r > 0 && n > 1 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))){
                read_child();
                dirty = 1;
            }
        }
        if(got){
            int r = yw_next_event(conn, &ev, 0);
            if(r < 0){
                break;
            }
            if(r > 0){
                if(ev.type == YW_EV_CLOSE){
                    break;
                } else if(ev.type == YW_EV_KEY){
                    key(&ev);
                } else if(ev.type == YW_EV_RESIZE){
                    dirty = 1;
                } else if(ev.type == YW_EV_MOUSE && ev.kind == YW_MOUSE_WHEEL && ev.wheel){
                    scroll_up += ev.wheel > 0 ? 3 : -3;
                    if(scroll_up < 0) scroll_up = 0;
                    dirty = 1;
                }
            }
        }
        if(child > 0 && child_fd < 0){
            child_done();
            dirty = 1;
        }
        if(dirty && !yw_pending(conn)){
            dirty = 0;
            render();
        }
    }
    if(child > 0){
        kill(child, SIGTERM);
        child_done();
    }
    yw_window_destroy(win);
    yw_disconnect(conn);
    return 0;
}
