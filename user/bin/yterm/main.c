/* yterm (T10.5): terminal grafic pentru ycomp, emulator VT100 / xterm (vezi vt.h) peste un pseudo-terminal din kernel.
 *   yterm [-s socket] [-t titlu] [-c coloane] [-r randuri] [-d director] [-x "comanda shell"] [-e program [arg...]]
 * Fara -x / -e porneste /bin/ysh. -x ruleaza o linie cu `ysh -c`, -e executa programul direct (cu argumentele ramase);
 * cand programul iese cu eroare fereastra ramane deschisa pana la o tasta, altfel se inchide.
 * Tastatura: Ctrl+litera, Alt = prefix ESC, sageti (si modul aplicatie), Home/End/Ins/Del/PgUp/PgDn, F1..F12;
 * Shift+PgUp / PgDn si rotita deruleaza istoricul (1000 de linii). Fereastra se redimensioneaza (TIOCSWINSZ + SIGWINCH). */
#include "ywin.h"
#include "vt.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/yinput.h>

#define FONT_MONO      YG_FONT_DIR "/DejaVuSansMono.ttf"
#define FONT_MONO_BOLD YG_FONT_DIR "/DejaVuSansMono-Bold.ttf"
#define FONT_PX        13.3f
#define PAD            4
#define SCROLLBACK     1000

enum {
    K_ESC = 1, K_BACKSPACE = 14, K_TAB = 15, K_ENTER = 28, K_F1 = 58, K_F12 = 69, K_HOME = 72, K_UP = 73, K_PGUP = 74,
    K_LEFT = 75, K_RIGHT = 76, K_END = 77, K_DOWN = 78, K_PGDN = 79, K_INS = 80, K_DEL = 81, K_SPACE = 56,
};

static YwConn *conn;
static YwWindow *win;
static YgFont *font, *bold;
static const YgTheme *th = &yg_theme_dark;
static Vt *vt;
static int master = -1;
static pid_t child;
static int cw, lh, asc;
static int view_off;            /* randuri derulate in sus in istoric */
static int dead;                /* programul a iesit cu eroare: asteapta o tasta */

static void to_master(void *u, const char *s, int n){
    (void)u;
    int off = 0;
    while(off < n && master >= 0){
        ssize_t w = write(master, s + off, (size_t)(n - off));
        if(w < 0){
            if(errno == EINTR){
                continue;
            }
            if(errno == EAGAIN){
                struct pollfd p = {master, POLLOUT, 0};
                poll(&p, 1, 100);
                continue;
            }
            return;
        }
        off += (int)w;
    }
}

static void on_title(void *u, const char *t){
    (void)u;
    if(win){
        yw_window_set_title(win, t);
    }
}

static int utf8(uint32_t cp, char *out){
    if(cp < 0x80){
        out[0] = (char)cp;
        return 1;
    }
    if(cp < 0x800){
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if(cp < 0x10000){
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static YgColor color_of(int idx, int is_fg, int bright_bold){
    if(idx == VT_DEFAULT){
        return is_fg ? th->text : th->background;
    }
    if(bright_bold && idx < 8){
        idx += 8;
    }
    return 0xFF000000u | vt_palette(idx);
}

/* Celula de pe randul vizual r (tinand cont de derulare) */
static VtCell view_cell(int r, int c){
    VtCell blank = {' ', VT_DEFAULT, VT_DEFAULT, 0};
    int line = vt->sb_n - view_off + r;
    if(line < vt->sb_n){
        int len;
        const VtCell *l = vt_sb_line(vt, line, &len);
        return l && c < len ? l[c] : blank;
    }
    return vt->cells[(line - vt->sb_n) * vt->cols + c];
}

static void draw_row(YgSurface *s, int r){
    int y = PAD + r * lh;
    int show_cursor = view_off == 0 && vt->cursor_visible && !dead;
    for(int c = 0; c < vt->cols; c++){
        VtCell cell = view_cell(r, c);
        int rev = (cell.attr & VT_A_REV) != 0;
        int cur = show_cursor && r == vt->y && c == vt->x;
        YgColor fg = color_of(cell.fg, 1, cell.attr & VT_A_BOLD);
        YgColor bg = color_of(cell.bg, 0, 0);
        if(rev != cur){
            YgColor t = fg;
            fg = bg;
            bg = t;
        }
        if(cell.attr & VT_A_DIM){
            fg = yg_mix(fg, bg, 110);
        }
        int x = PAD + c * cw;
        yg_fill_rect(s, x, y, cw, lh, bg);
        if(cell.ch > ' ' && cell.ch != 0x7F){
            char u[5];
            int n = utf8(cell.ch, u);
            u[n] = 0;
            yg_draw_text(s, (cell.attr & VT_A_BOLD) && bold ? bold : font, FONT_PX, x, y + asc + 1, u, fg);
        }
        if(cell.attr & VT_A_UNDER){
            yg_fill_rect(s, x, y + lh - 2, cw, 1, fg);
        }
    }
    /* marginea dreapta (ce nu incape in celule) */
    int used = PAD + vt->cols * cw;
    if(used < s->width){
        yg_fill_rect(s, used, y, s->width - used, lh, th->background);
    }
}

static void render(int all){
    YgSurface *s = yw_window_surface(win);
    if(all || view_off){
        yg_clear(s, th->background);
        vt_mark_all_dirty(vt);
    }
    int first = -1, last = -1;
    for(int r = 0; r < vt->rows; r++){
        if(vt->dirty[r]){
            draw_row(s, r);
            vt->dirty[r] = 0;
            if(first < 0){
                first = r;
            }
            last = r;
        }
    }
    if(view_off){
        int h = s->height;
        int total = vt->sb_n + vt->rows;
        int bar_h = h * vt->rows / total;
        if(bar_h < 12){
            bar_h = 12;
        }
        int bar_y = (h - bar_h) * (vt->sb_n - view_off) / (vt->sb_n ? vt->sb_n : 1);
        yg_fill_rect(s, s->width - 4, bar_y, 3, bar_h, yg_mix(th->background, th->text_muted, 160));
    }
    if(first < 0 && !all){
        return;
    }
    if(all || view_off){
        yw_window_damage_all(win);
    } else {
        int y0 = first == 0 ? 0 : PAD + first * lh;
        int y1 = last == vt->rows - 1 ? s->height : PAD + (last + 1) * lh;
        yw_window_damage(win, 0, y0, s->width, y1 - y0);
    }
}

static void fit_to_window(void){
    int w, h;
    yw_window_size(win, &w, &h);
    int cols = (w - 2 * PAD) / cw, rows = (h - 2 * PAD) / lh;
    if(cols < 2){
        cols = 2;
    }
    if(rows < 1){
        rows = 1;
    }
    if(cols != vt->cols || rows != vt->rows){
        vt_resize(vt, cols, rows);
        struct winsize ws = {(unsigned short)rows, (unsigned short)cols, (unsigned short)w, (unsigned short)h};
        if(master >= 0){
            ioctl(master, TIOCSWINSZ, &ws);
        }
    }
    if(view_off > vt->sb_n){
        view_off = vt->sb_n;
    }
}

/* Tasta -> octeti pentru program; numarul de octeti */
static int key_bytes(const YwEvent *ev, char *out){
    int k = ev->keycode, alt = (ev->modifiers & YK_MOD_ALT) != 0, ctrl = (ev->modifiers & YK_MOD_CTRL) != 0;
    int shift = (ev->modifiers & YK_MOD_SHIFT) != 0;
    const char *seq = NULL;
    switch(k){
    case K_ENTER: out[0] = '\r'; return 1;
    case K_BACKSPACE: out[0] = 0x7F; return 1;
    case K_TAB:
        if(shift){
            seq = "\x1b[Z";
            break;
        }
        out[0] = '\t';
        return 1;
    case K_ESC: out[0] = 0x1B; return 1;
    case K_UP: seq = vt->app_cursor ? "\x1bOA" : "\x1b[A"; break;
    case K_DOWN: seq = vt->app_cursor ? "\x1bOB" : "\x1b[B"; break;
    case K_RIGHT: seq = vt->app_cursor ? "\x1bOC" : "\x1b[C"; break;
    case K_LEFT: seq = vt->app_cursor ? "\x1bOD" : "\x1b[D"; break;
    case K_HOME: seq = vt->app_cursor ? "\x1bOH" : "\x1b[H"; break;
    case K_END: seq = vt->app_cursor ? "\x1bOF" : "\x1b[F"; break;
    case K_INS: seq = "\x1b[2~"; break;
    case K_DEL: seq = "\x1b[3~"; break;
    case K_PGUP: seq = "\x1b[5~"; break;
    case K_PGDN: seq = "\x1b[6~"; break;
    default:
        break;
    }
    if(!seq && k >= K_F1 && k <= K_F12){
        static const char *fk[12] = {"\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS", "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~",
                                     "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~"};
        seq = fk[k - K_F1];
    }
    if(seq){
        int n = (int)strlen(seq);
        memcpy(out, seq, (size_t)n);
        return n;
    }
    int a = ev->ascii;
    if(k == K_SPACE && ctrl){
        out[0] = 0;
        return 1;
    }
    if(a <= 0 || a >= 127){
        return 0;
    }
    if(ctrl){
        if((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z')){
            a = (a | 0x20) - 'a' + 1;
        } else if(a == '[' || a == '\\' || a == ']' || a == '^' || a == '_'){
            a = a - '[' + 0x1B;
        } else if(a == '@'){
            a = 0;
        } else {
            return 0;
        }
    }
    int n = 0;
    if(alt){
        out[n++] = 0x1B;
    }
    out[n++] = (char)a;
    return n;
}

static void on_key(const YwEvent *ev){
    if(!ev->pressed){
        return;
    }
    if(dead){
        dead = 2;                       /* orice tasta inchide */
        return;
    }
    int shift = (ev->modifiers & YK_MOD_SHIFT) != 0;
    if(shift && (ev->keycode == K_PGUP || ev->keycode == K_PGDN)){
        int page = vt->rows > 2 ? vt->rows - 1 : 1;
        view_off += ev->keycode == K_PGUP ? page : -page;
        if(view_off < 0){
            view_off = 0;
        }
        if(view_off > vt->sb_n){
            view_off = vt->sb_n;
        }
        render(1);
        return;
    }
    char buf[16];
    int n = key_bytes(ev, buf);
    if(n <= 0){
        return;
    }
    if(view_off){
        view_off = 0;
        render(1);
    }
    to_master(NULL, buf, n);
}

static const char *startdir;

static void make_child(const char *shellcmd, char **prog, const struct winsize *ws){
    int sl;
    if(openpty(&master, &sl, NULL, NULL, ws) < 0){
        perror("yterm: openpty");
        exit(1);
    }
    child = fork();
    if(child < 0){
        perror("yterm: fork");
        exit(1);
    }
    if(child == 0){
        close(master);
        close(yw_fd(conn));
        dup2(sl, 0);
        dup2(sl, 1);
        dup2(sl, 2);
        if(sl > 2){
            close(sl);
        }
        const char *home = getenv("HOME");
        if(!(startdir && chdir(startdir) == 0) && (!home || chdir(home) < 0)){
            chdir("/");
        }
        setenv("TERM", "xterm-256color", 1);
        char num[16];
        snprintf(num, sizeof(num), "%d", ws->ws_col);
        setenv("COLUMNS", num, 1);
        snprintf(num, sizeof(num), "%d", ws->ws_row);
        setenv("LINES", num, 1);
        if(prog){
            char path[128];
            const char *p = prog[0];
            if(!strchr(p, '/')){
                snprintf(path, sizeof(path), "/bin/%s", p);
                p = path;
            }
            execv(p, prog);
            printf("yterm: %s: %s\n", prog[0], strerror(errno));
        } else if(shellcmd){
            char *av[] = {"ysh", "-c", (char *)shellcmd, NULL};
            execv("/bin/ysh", av);
        } else {
            char *av[] = {"ysh", NULL};
            execv("/bin/ysh", av);
        }
        _exit(127);
    }
    close(sl);
    fcntl(master, F_SETFL, O_NONBLOCK);
}

static void stop_child(void){
    if(child <= 0){
        return;
    }
    int st;
    kill(child, SIGHUP);
    if(master >= 0){
        close(master);
        master = -1;
    }
    for(int i = 0; i < 40; i++){
        if(waitpid(child, &st, WNOHANG) == child){
            child = 0;
            return;
        }
        struct timespec ts = {0, 25000000};
        nanosleep(&ts, NULL);
    }
    kill(child, SIGKILL);
    waitpid(child, &st, 0);
    child = 0;
}

static int usage(void){
    printf("uz: yterm [-s socket] [-t titlu] [-c coloane] [-r randuri] [-d director] [-x \"comanda\"] [-e program [arg...]]\n");
    return 2;
}

int main(int argc, char **argv){
    const char *sock = NULL, *title = "Terminal", *shellcmd = NULL;
    char **prog = NULL;
    int cols = 80, rows = 24;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            usage();
            return 0;
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            sock = argv[++i];
        } else if(!strcmp(argv[i], "-t") && i + 1 < argc){
            title = argv[++i];
        } else if(!strcmp(argv[i], "-c") && i + 1 < argc){
            cols = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-r") && i + 1 < argc){
            rows = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-d") && i + 1 < argc){
            startdir = argv[++i];
        } else if(!strcmp(argv[i], "-x") && i + 1 < argc){
            shellcmd = argv[++i];
        } else if(!strcmp(argv[i], "-e") && i + 1 < argc){
            prog = &argv[i + 1];
            break;
        } else {
            return usage();
        }
    }
    if(cols < 10 || cols > 400 || rows < 3 || rows > 200){
        return usage();
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
    int32_t q[6];
    if(yw_query(conn, YW_Q_THEME, 0, 0, q) == 0 && q[0]){
        th = &yg_theme_light;
    }
    font = yg_font_load(FONT_MONO);
    if(!font){
        font = yg_font_load(YG_FONT_REGULAR);
    }
    bold = yg_font_load(FONT_MONO_BOLD);
    if(!font){
        fprintf(stderr, "yterm: fara font\n");
        return 1;
    }
    int desc, gap;
    yg_font_vmetrics(font, FONT_PX, &asc, &desc, &gap);
    cw = yg_text_width(font, FONT_PX, "M");
    if(cw < 4){
        cw = 8;
    }
    lh = asc - desc + 2;
    if(cols * cw + 2 * PAD > yw_screen_width(conn)){
        cols = (yw_screen_width(conn) - 2 * PAD - 8) / cw;
    }
    if(rows * lh + 2 * PAD > yw_screen_height(conn) - 80){
        rows = (yw_screen_height(conn) - 80 - 2 * PAD) / lh;
    }
    int ww = cols * cw + 2 * PAD, wh = rows * lh + 2 * PAD;
    win = yw_window_new(conn, ww, wh, title);
    if(!win){
        perror("yterm: fereastra");
        return 1;
    }
    VtOps ops = {to_master, on_title, NULL, NULL};
    vt = vt_new(cols, rows, SCROLLBACK, &ops);
    if(!vt){
        fprintf(stderr, "yterm: memorie\n");
        return 1;
    }
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, (unsigned short)ww, (unsigned short)wh};
    signal(SIGPIPE, SIG_IGN);
    make_child(shellcmd, prog, &ws);
    render(1);

    int status = 0;
    for(;;){
        struct pollfd pf[2];
        pf[0].fd = yw_fd(conn);
        pf[0].events = POLLIN;
        pf[1].fd = master;
        pf[1].events = POLLIN;
        int got = yw_pending(conn) > 0;
        int mdata = 0;
        if(!got){
            int r = poll(pf, master >= 0 ? 2 : 1, -1);
            if(r < 0 && errno != EINTR){
                break;
            }
            if(r > 0){
                got = (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
                mdata = master >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR));
            }
        }
        if(mdata){
            char buf[2048];
            int total = 0, eof = 0;
            while(total < 16384){
                ssize_t n = read(master, buf, sizeof(buf));
                if(n > 0){
                    vt_feed(vt, buf, (int)n);
                    total += (int)n;
                    continue;
                }
                if(n == 0 || (errno != EAGAIN && errno != EINTR)){
                    eof = 1;
                }
                break;
            }
            if(total){
                if(view_off == 0){
                    render(0);
                } else {
                    vt_mark_all_dirty(vt);
                }
            }
            if(eof){
                close(master);
                master = -1;
                kill(child, SIGHUP);
                waitpid(child, &status, 0);
                child = 0;
                if(status != 0 && !shellcmd){
                    char msg[80];
                    snprintf(msg, sizeof(msg), "\r\n[terminat, cod %d - orice tasta inchide]", (status >> 8) & 0xFF);
                    vt_feed(vt, msg, (int)strlen(msg));
                    dead = 1;
                    render(0);
                } else {
                    break;
                }
            }
        }
        if(got){
            YwEvent ev;
            int r = yw_next_event(conn, &ev, 0);
            if(r < 0){
                break;
            }
            if(r > 0){
                if(ev.type == YW_EV_CLOSE){
                    break;
                } else if(ev.type == YW_EV_KEY){
                    on_key(&ev);
                    if(dead == 2){
                        break;
                    }
                } else if(ev.type == YW_EV_RESIZE){
                    fit_to_window();
                    render(1);
                } else if(ev.type == YW_EV_THEME){
                    th = ev.light ? &yg_theme_light : &yg_theme_dark;
                    render(1);
                } else if(ev.type == YW_EV_MOUSE && ev.kind == YW_MOUSE_WHEEL && ev.wheel){
                    int old = view_off;
                    view_off += ev.wheel > 0 ? 3 : -3;
                    if(view_off < 0){
                        view_off = 0;
                    }
                    if(view_off > vt->sb_n){
                        view_off = vt->sb_n;
                    }
                    if(old != view_off){
                        render(1);
                    }
                }
            }
        }
    }
    stop_child();
    yw_window_destroy(win);
    yw_disconnect(conn);
    return 0;
}
