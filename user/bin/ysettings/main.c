/* ysettings (T10.5): setari grafice pentru YokaiOS, in ferestra ycomp.
 *   ysettings [-s socket] [-p pagina 1..5]
 * Pagini (stanga; clic sau F1..F5 / Ctrl+PgUp / Ctrl+PgDn):
 *   1 Aspect: tema Dark / Light (se aplica pe loc tuturor aplicatiilor, YW_SETTHEME; se salveaza in /etc/ycomp.theme daca radacina
 *     e scriibila, altfel doar pana la repornire). Stanga / Dreapta / T schimba tema.
 *   2 Data si ora: ceasul UTC (fara fus local); campuri An / Luna / Zi / Ora / Min / Sec cu - / +, Aplica = settimeofday (cere root).
 *     Stanga / Dreapta alege campul, Sus / Jos schimba valoarea (PgUp / PgDn de 10 ori), Enter aplica, R resincronizeaza.
 *   3 Retea: interfetele (adresa, masca, MAC, MTU, trafic), poarta implicita, DNS, test de ping spre poarta (P), R reimprospateaza.
 *   4 Utilizatori: /etc/passwd, utilizatorul curent, "Schimba parola" (yterm -e passwd).
 *   5 Telemetrie drona: pagina goala, rezervata pentru mai tarziu. */
#include "ywin.h"
#include "yui.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/yinput.h>

#define NAV_W   150
#define NPAGES  5
#define THEME_FILE "/etc/ycomp.theme"

enum { K_ESC = 1, K_TAB = 15, K_ENTER = 28, K_F1 = 58, K_UP = 73, K_PGUP = 74, K_LEFT = 75, K_RIGHT = 76, K_DOWN = 78,
       K_PGDN = 79 };

static const char *page_names[NPAGES] = {"Aspect", "Dată și oră", "Rețea", "Utilizatori", "Telemetrie dronă"};

static YwConn *conn;
static YwWindow *win;
static YUi ui;
static const YgTheme *th = &yg_theme_dark;
static int page;
static int dirty = 1;
static int hover_kind, hover_idx = -1;           /* 1 = nav, 2 = buton de pagina, 3 = camp, 4 = card */
static char status[160];

/* data / ora */
static int dt[6];                               /* an, luna, zi, ora, min, sec */
static int dt_focus;
static int dt_edited;
static const char *dt_names[6] = {"An", "Lună", "Zi", "Oră", "Minute", "Secunde"};

/* retea */
typedef struct {
    char name[16];
    char addr[20], mask[20], mac[20];
    int up, mtu;
    unsigned long long rx, tx;
} If;
static If ifs[6];
static int nifs;
static char gateway[20], dns[2][20];
static char ping_result[96];

/* utilizatori */
typedef struct {
    char name[32], home[64], shell[32];
    int uid, gid;
} User;
static User users[16];
static int nusers;
static int user_sel;

static void set_status(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    dirty = 1;
}

/* ---------------- tema ---------------- */

static void apply_theme(int light){
    yw_set_theme(conn, light);
    FILE *f = fopen(THEME_FILE, "w");
    if(f){
        fputs(light ? "light\n" : "dark\n", f);
        fclose(f);
        set_status("Tema %s aplicată și salvată", light ? "Luminos" : "Întunecat");
    } else {
        set_status("Tema %s aplicată (rădăcina e doar-citire: se pierde la repornire)", light ? "Luminos" : "Întunecat");
    }
}

/* ---------------- data si ora ---------------- */

static int days_in(int y, int m){
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)){
        return 29;
    }
    return d[(m - 1) % 12];
}

static void dt_load_now(void){
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    dt[0] = tm.tm_year + 1900;
    dt[1] = tm.tm_mon + 1;
    dt[2] = tm.tm_mday;
    dt[3] = tm.tm_hour;
    dt[4] = tm.tm_min;
    dt[5] = tm.tm_sec;
}

static void dt_clamp(void){
    if(dt[0] < 1970){
        dt[0] = 1970;
    }
    if(dt[0] > 2200){
        dt[0] = 2200;
    }
    if(dt[1] < 1){
        dt[1] = 12;
    }
    if(dt[1] > 12){
        dt[1] = 1;
    }
    int dm = days_in(dt[0], dt[1]);
    if(dt[2] < 1){
        dt[2] = dm;
    }
    if(dt[2] > dm){
        dt[2] = 1;
    }
    if(dt[3] < 0){
        dt[3] = 23;
    }
    if(dt[3] > 23){
        dt[3] = 0;
    }
    if(dt[4] < 0){
        dt[4] = 59;
    }
    if(dt[4] > 59){
        dt[4] = 0;
    }
    if(dt[5] < 0){
        dt[5] = 59;
    }
    if(dt[5] > 59){
        dt[5] = 0;
    }
}

static void dt_step(int field, int delta){
    dt[field] += delta;
    dt_edited = 1;
    dt_clamp();
    dirty = 1;
}

static void dt_apply(void){
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = dt[0] - 1900;
    tm.tm_mon = dt[1] - 1;
    tm.tm_mday = dt[2];
    tm.tm_hour = dt[3];
    tm.tm_min = dt[4];
    tm.tm_sec = dt[5];
    struct timeval tv = {timegm(&tm), 0};
    if(settimeofday(&tv, NULL) < 0){
        set_status("Nu s-a putut seta ora: %s%s", strerror(errno), errno == EPERM ? " (cere root)" : "");
        return;
    }
    dt_edited = 0;
    set_status("Ora a fost setată: %04d-%02d-%02d %02d:%02d:%02d UTC", dt[0], dt[1], dt[2], dt[3], dt[4], dt[5]);
}

/* ---------------- retea ---------------- */

static void sin_str(const struct sockaddr *sa, char *out, size_t cap){
    const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
    inet_ntop(AF_INET, &s->sin_addr, out, (socklen_t)cap);
}

static int if_ioctl(int sock, const char *name, unsigned long cmd, struct ifreq *r){
    memset(r, 0, sizeof(*r));
    snprintf(r->ifr_name, IFNAMSIZ, "%s", name);
    return ioctl(sock, cmd, r);
}

static void net_load(void){
    nifs = 0;
    gateway[0] = dns[0][0] = dns[1][0] = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    FILE *f = fopen("/proc/net/dev", "r");
    char line[256];
    while(f && fgets(line, sizeof(line), f) && nifs < 6){
        char *colon = strchr(line, ':');
        if(!colon){
            continue;
        }
        *colon = 0;
        char *name = line;
        while(*name == ' '){
            name++;
        }
        If *e = &ifs[nifs];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name);
        unsigned long long v[16] = {0};
        char *p = colon + 1;
        for(int i = 0; i < 16; i++){
            v[i] = strtoull(p, &p, 10);
        }
        e->rx = v[0];
        e->tx = v[8];
        strcpy(e->addr, "-");
        strcpy(e->mask, "-");
        strcpy(e->mac, "-");
        if(sock >= 0){
            struct ifreq r;
            if(if_ioctl(sock, e->name, SIOCGIFFLAGS, &r) == 0){
                e->up = (r.ifr_flags & IFF_UP) != 0;
            }
            if(if_ioctl(sock, e->name, SIOCGIFADDR, &r) == 0){
                sin_str(&r.ifr_addr, e->addr, sizeof(e->addr));
            }
            if(if_ioctl(sock, e->name, SIOCGIFNETMASK, &r) == 0){
                sin_str(&r.ifr_addr, e->mask, sizeof(e->mask));
            }
            if(if_ioctl(sock, e->name, SIOCGIFMTU, &r) == 0){
                e->mtu = r.ifr_mtu;
            }
            if(if_ioctl(sock, e->name, SIOCGIFHWADDR, &r) == 0){
                const unsigned char *m = (const unsigned char *)r.ifr_hwaddr.sa_data;
                snprintf(e->mac, sizeof(e->mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
        }
        nifs++;
    }
    if(f){
        fclose(f);
    }
    if(sock >= 0){
        close(sock);
    }
    f = fopen("/proc/net/route", "r");
    while(f && fgets(line, sizeof(line), f)){
        char ifn[16];
        unsigned dest, gw;
        if(sscanf(line, "%15s %x %x", ifn, &dest, &gw) == 3 && dest == 0 && gw != 0){
            snprintf(gateway, sizeof(gateway), "%u.%u.%u.%u", gw & 255, (gw >> 8) & 255, (gw >> 16) & 255, (gw >> 24) & 255);
            break;
        }
    }
    if(f){
        fclose(f);
    }
    f = fopen("/etc/resolv.conf", "r");
    int nd = 0;
    while(f && fgets(line, sizeof(line), f) && nd < 2){
        char a[32];
        if(sscanf(line, "nameserver %31s", a) == 1){
            snprintf(dns[nd++], sizeof(dns[0]), "%s", a);
        }
    }
    if(f){
        fclose(f);
    }
    dirty = 1;
}

/* ping -c 1 spre poarta; blocheaza cel mult ~3 s */
static void net_ping(void){
    const char *target = gateway[0] ? gateway : "10.0.2.2";
    pid_t pid = fork();
    if(pid < 0){
        set_status("fork: %s", strerror(errno));
        return;
    }
    if(pid == 0){
        close(yw_fd(conn));
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        char *av[] = {"ping", "-c", "1", "-W", "2", (char *)target, NULL};
        execv("/bin/ping", av);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if(WIFEXITED(st) && WEXITSTATUS(st) == 0){
        snprintf(ping_result, sizeof(ping_result), "%s răspunde", target);
    } else {
        snprintf(ping_result, sizeof(ping_result), "%s nu răspunde", target);
    }
    dirty = 1;
}

/* ---------------- utilizatori ---------------- */

static void users_load(void){
    nusers = 0;
    FILE *f = fopen("/etc/passwd", "r");
    char line[256];
    while(f && fgets(line, sizeof(line), f) && nusers < 16){
        char *fl[7];
        int n = 0;
        line[strcspn(line, "\r\n")] = 0;
        fl[n++] = line;
        for(char *p = line; *p && n < 7; p++){
            if(*p == ':'){
                *p = 0;
                fl[n++] = p + 1;
            }
        }
        if(n < 7){
            continue;
        }
        User *u = &users[nusers++];
        snprintf(u->name, sizeof(u->name), "%s", fl[0]);
        u->uid = atoi(fl[2]);
        u->gid = atoi(fl[3]);
        snprintf(u->home, sizeof(u->home), "%s", fl[5]);
        snprintf(u->shell, sizeof(u->shell), "%s", fl[6]);
    }
    if(f){
        fclose(f);
    }
    for(int i = 0; i < nusers; i++){
        if(users[i].uid == (int)getuid()){
            user_sel = i;
        }
    }
}

static void change_password(void){
    if(user_sel < 0 || user_sel >= nusers){
        return;
    }
    pid_t pid = fork();
    if(pid == 0){
        close(yw_fd(conn));
        char title[64];
        snprintf(title, sizeof(title), "Parola - %s", users[user_sel].name);
        char *av[] = {"yterm", "-t", title, "-c", "60", "-r", "10", "-e", "/bin/passwd", users[user_sel].name, NULL};
        execv("/bin/yterm", av);
        _exit(127);
    }
    set_status("Schimbarea parolei pentru %s s-a deschis într-un terminal", users[user_sel].name);
}

/* ---------------- geometrie ---------------- */

static YgRect nav_rect(int i){
    return yg_rect(8, 12 + i * 34, NAV_W - 16, 30);
}

static YgRect content_rect(void){
    YgSurface *s = yw_window_surface(win);
    return yg_rect(NAV_W, 0, s->width - NAV_W, s->height - 26);
}

static YgRect theme_card(int light){
    YgRect c = content_rect();
    int w = (c.w - 60) / 2;
    if(w > 220){
        w = 220;
    }
    return yg_rect(c.x + 20 + (light ? w + 20 : 0), 70, w, 150);
}

static YgRect dt_btn(int field, int plus){
    YgRect c = content_rect();
    int col = field / 3, row = field % 3;
    int x0 = c.x + 20 + col * ((c.w - 40) / 2);
    int y = 110 + row * 40;
    return plus ? yg_rect(x0 + 160, y, 30, 28) : yg_rect(x0 + 90, y, 30, 28);
}

static YgRect dt_field_rect(int field){
    YgRect m = dt_btn(field, 0);
    return yg_rect(m.x + 34, m.y, 52, 28);
}

static YgRect action_btn(int i){
    YgRect c = content_rect();
    return yg_rect(c.x + 20 + i * 150, 250, 140, 30);
}

static YgRect pw_btn(void){
    YgRect c = content_rect();
    return yg_rect(c.x + 20, c.h - 48, 160, 30);
}

static int net_cards_end(void){
    return 48 + nifs * 126;
}

/* ---------------- desen ---------------- */

static void card_preview(YgRect r, const YgTheme *t, int selected, const char *label){
    YgSurface *s = ui.s;
    yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, selected ? th->accent : th->border);
    YgRect in = yg_rect(r.x + 3, r.y + 3, r.w - 6, r.h - 6);
    yg_fill_rounded_rect(s, in.x, in.y, in.w, in.h, 6, t->background);
    yg_fill_rounded_rect(s, in.x + 14, in.y + 14, in.w - 28, in.h - 60, 4, t->surface);
    yg_draw_rounded_rect(s, in.x + 14, in.y + 14, in.w - 28, in.h - 60, 4, 1, t->border);
    yg_fill_rect(s, in.x + 14, in.y + 14, in.w - 28, 16, t->surface_alt);
    yg_fill_rounded_rect(s, in.x + 22, in.y + 42, 50, 14, 4, t->accent);
    yg_fill_rect(s, in.x + 22, in.y + 64, in.w - 60, 4, t->text_muted);
    yg_fill_rect(s, in.x + 22, in.y + 74, in.w - 90, 4, t->text_muted);
    yg_fill_rect(s, in.x, in.y + in.h - 22, in.w, 22, t->surface_alt);
    yg_draw_logo(s, in.x + in.w / 2.0f, in.y + in.h - 11.0f, 18, t->logo);
    yui_text(&ui, 13, yg_rect(r.x, r.y + r.h + 4, r.w, 22), label, th->text, YUI_CENTER, selected);
}

static void draw_value(YgRect r, const char *label, const char *value){
    yui_text(&ui, 13, yg_rect(r.x, r.y, 120, r.h), label, th->text_muted, YUI_LEFT, 0);
    yui_text(&ui, 13, yg_rect(r.x + 120, r.y, r.w - 120, r.h), value, th->text, YUI_LEFT, 0);
}

static void fmt_bytes(unsigned long long n, char *out, size_t cap){
    if(n < 1024){
        snprintf(out, cap, "%llu B", n);
    } else if(n < 1024 * 1024){
        snprintf(out, cap, "%llu.%llu KiB", n / 1024, (n % 1024) * 10 / 1024);
    } else {
        snprintf(out, cap, "%llu.%llu MiB", n / (1024 * 1024), (n % (1024 * 1024)) * 10 / (1024 * 1024));
    }
}

static void draw_page(void){
    YgSurface *s = ui.s;
    YgRect c = content_rect();
    yui_text(&ui, 20, yg_rect(c.x + 14, 12, c.w - 28, 34), page_names[page], th->text, YUI_LEFT, 1);
    if(page == 0){
        int light = th == &yg_theme_light;
        yui_text(&ui, 13, yg_rect(c.x + 14, 44, c.w - 28, 22), "Tema se aplică pe loc tuturor ferestrelor.", th->text_muted, YUI_LEFT, 0);
        card_preview(theme_card(0), &yg_theme_dark, !light, "Întunecat (Dark)");
        card_preview(theme_card(1), &yg_theme_light, light, "Luminos (Light)");
        yui_text(&ui, 12, yg_rect(c.x + 14, 250, c.w - 28, 22), "Stânga / Dreapta sau T schimbă tema.", th->text_muted, YUI_LEFT, 0);
    } else if(page == 1){
        char now[64];
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(now, sizeof(now), "Acum: %Y-%m-%d %H:%M:%S UTC", &tm);
        yui_text(&ui, 14, yg_rect(c.x + 14, 46, c.w - 28, 26), now, th->text, YUI_LEFT, 0);
        yui_text(&ui, 12, yg_rect(c.x + 14, 72, c.w - 28, 22), "Fus orar: UTC (nu există fus local).", th->text_muted, YUI_LEFT, 0);
        for(int i = 0; i < 6; i++){
            YgRect m = dt_btn(i, 0), p = dt_btn(i, 1), fr = dt_field_rect(i);
            int x0 = c.x + 20 + (i / 3) * ((c.w - 40) / 2);
            yui_text(&ui, 13, yg_rect(x0, m.y, 90, m.h), dt_names[i], i == dt_focus ? th->accent : th->text_muted, YUI_LEFT, i == dt_focus);
            yui_button(&ui, m, "-", hover_kind == 2 && hover_idx == i * 2 ? 1 : 0, 1);
            yui_button(&ui, p, "+", hover_kind == 2 && hover_idx == i * 2 + 1 ? 1 : 0, 1);
            char v[16];
            snprintf(v, sizeof(v), i == 0 ? "%04d" : "%02d", dt[i]);
            yg_fill_rounded_rect(s, fr.x, fr.y, fr.w, fr.h, 4, th->surface_alt);
            yg_draw_rounded_rect(s, fr.x, fr.y, fr.w, fr.h, 4, 1, i == dt_focus ? th->accent : th->border);
            yui_text(&ui, 14, fr, v, th->text, YUI_CENTER, 0);
        }
        yui_button(&ui, action_btn(0), "Aplică", hover_kind == 2 && hover_idx == 20 ? 1 : 2, 1);
        yui_button(&ui, action_btn(1), "Resincronizează", hover_kind == 2 && hover_idx == 21 ? 1 : 0, 1);
        yui_text(&ui, 12, yg_rect(c.x + 14, 292, c.w - 28, 22),
                 "Sus / Jos schimbă valoarea, Stânga / Dreapta alege câmpul, Enter aplică.", th->text_muted, YUI_LEFT, 0);
    } else if(page == 2){
        int y = 48;
        for(int i = 0; i < nifs; i++){
            If *e = &ifs[i];
            YgRect card = yg_rect(c.x + 14, y, c.w - 28, 118);
            yg_fill_rounded_rect(s, card.x, card.y, card.w, card.h, 8, th->surface_alt);
            yg_draw_rounded_rect(s, card.x, card.y, card.w, card.h, 8, 1, th->border);
            yui_text(&ui, 14, yg_rect(card.x + 6, card.y + 2, 120, 26), e->name, th->text, YUI_LEFT, 1);
            yg_fill_circle(s, card.x + card.w - 18.0f, card.y + 15.0f, 5, e->up ? YG_RGB(60, 190, 90) : th->text_muted);
            yui_text(&ui, 12, yg_rect(card.x + card.w - 110, card.y + 2, 80, 26), e->up ? "activă" : "inactivă", th->text_muted, YUI_RIGHT, 0);
            char tx[64], rx[64], both[140], mtu[16];
            fmt_bytes(e->rx, rx, sizeof(rx));
            fmt_bytes(e->tx, tx, sizeof(tx));
            snprintf(both, sizeof(both), "primit %s, trimis %s", rx, tx);
            snprintf(mtu, sizeof(mtu), "%d", e->mtu);
            draw_value(yg_rect(card.x + 8, card.y + 28, card.w - 16, 18), "Adresă IP", e->addr);
            draw_value(yg_rect(card.x + 8, card.y + 46, card.w - 16, 18), "Mască", e->mask);
            draw_value(yg_rect(card.x + 8, card.y + 64, card.w - 16, 18), "MAC", e->mac);
            draw_value(yg_rect(card.x + 8, card.y + 82, card.w - 16, 18), "MTU", e->mtu ? mtu : "-");
            draw_value(yg_rect(card.x + 8, card.y + 100, card.w - 16, 16), "Trafic", both);
            y += 126;
        }
        if(!nifs){
            yui_text(&ui, 13, yg_rect(c.x + 14, 52, c.w - 28, 24), "Nu există interfețe de rețea.", th->text_muted, YUI_LEFT, 0);
        }
        draw_value(yg_rect(c.x + 20, y + 2, c.w - 40, 20), "Poartă implicită", gateway[0] ? gateway : "-");
        char d[48];
        snprintf(d, sizeof(d), "%s%s%s", dns[0][0] ? dns[0] : "-", dns[1][0] ? ", " : "", dns[1]);
        draw_value(yg_rect(c.x + 20, y + 22, c.w - 40, 20), "DNS", d);
        yui_button(&ui, yg_rect(c.x + 20, y + 52, 170, 28), "Testează conexiunea", hover_kind == 2 && hover_idx == 30 ? 1 : 0, 1);
        yui_button(&ui, yg_rect(c.x + 200, y + 52, 130, 28), "Reîmprospătează", hover_kind == 2 && hover_idx == 31 ? 1 : 0, 1);
        if(ping_result[0]){
            yui_text(&ui, 13, yg_rect(c.x + 340, y + 52, c.w - 350, 28), ping_result, th->text, YUI_LEFT, 0);
        }
    } else if(page == 3){
        yg_fill_rect(s, c.x + 14, 52, c.w - 28, 22, th->surface_alt);
        yui_text(&ui, 12, yg_rect(c.x + 14, 52, 120, 22), "Nume", th->text_muted, YUI_LEFT, 1);
        yui_text(&ui, 12, yg_rect(c.x + 134, 52, 60, 22), "UID", th->text_muted, YUI_LEFT, 1);
        yui_text(&ui, 12, yg_rect(c.x + 194, 52, 60, 22), "GID", th->text_muted, YUI_LEFT, 1);
        yui_text(&ui, 12, yg_rect(c.x + 254, 52, 140, 22), "Director", th->text_muted, YUI_LEFT, 1);
        yui_text(&ui, 12, yg_rect(c.x + 394, 52, c.w - 410, 22), "Shell", th->text_muted, YUI_LEFT, 1);
        for(int i = 0; i < nusers; i++){
            YgRect r = yg_rect(c.x + 14, 76 + i * 26, c.w - 28, 26);
            int sel = i == user_sel;
            if(sel){
                yg_fill_rect(s, r.x, r.y, r.w, r.h, th->accent);
            } else if(hover_kind == 2 && hover_idx == 100 + i){
                yg_fill_rect(s, r.x, r.y, r.w, r.h, yg_mix(th->surface, th->text, 22));
            }
            YgColor fg = sel ? th->accent_text : th->text;
            char n[48], u[12], g[12];
            snprintf(n, sizeof(n), "%s%s", users[i].name, users[i].uid == (int)getuid() ? " (tu)" : "");
            snprintf(u, sizeof(u), "%d", users[i].uid);
            snprintf(g, sizeof(g), "%d", users[i].gid);
            yui_text(&ui, 13, yg_rect(r.x, r.y, 120, r.h), n, fg, YUI_LEFT, 0);
            yui_text(&ui, 13, yg_rect(r.x + 120, r.y, 60, r.h), u, fg, YUI_LEFT, 0);
            yui_text(&ui, 13, yg_rect(r.x + 180, r.y, 60, r.h), g, fg, YUI_LEFT, 0);
            yui_text(&ui, 13, yg_rect(r.x + 240, r.y, 140, r.h), users[i].home, fg, YUI_LEFT, 0);
            yui_text(&ui, 13, yg_rect(r.x + 380, r.y, r.w - 380, r.h), users[i].shell, fg, YUI_LEFT, 0);
        }
        char me[96];
        const char *who = "?";
        for(int i = 0; i < nusers; i++){
            if(users[i].uid == (int)getuid()){
                who = users[i].name;
            }
        }
        snprintf(me, sizeof(me), "Utilizator curent: %s (uid %d)", who, (int)getuid());
        yui_text(&ui, 13, yg_rect(c.x + 14, c.h - 80, c.w - 28, 24), me, th->text_muted, YUI_LEFT, 0);
        yui_button(&ui, pw_btn(), "Schimbă parola", hover_kind == 2 && hover_idx == 40 ? 1 : 0, nusers > 0);
    } else {
        YgRect m = yg_rect(c.x, 100, c.w, c.h - 140);
        yg_draw_logo(s, m.x + m.w / 2.0f, m.y + 70.0f, 110, YG_WITH_ALPHA(th->logo, 70));
        yui_text(&ui, 16, yg_rect(m.x, m.y + 140, m.w, 30), "Telemetrie dronă", th->text_muted, YUI_CENTER, 1);
        yui_text(&ui, 13, yg_rect(m.x, m.y + 172, m.w, 24), "Pagină rezervată pentru mai târziu.", th->text_muted, YUI_CENTER, 0);
    }
}

static void draw_all(void){
    YgSurface *s = yw_window_surface(win);
    ui.s = s;
    ui.t = th;
    yg_clear(s, th->surface);
    yg_fill_rect(s, 0, 0, NAV_W, s->height, th->surface_alt);
    yg_fill_rect(s, NAV_W - 1, 0, 1, s->height, th->border);
    for(int i = 0; i < NPAGES; i++){
        YgRect r = nav_rect(i);
        if(i == page){
            yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, th->accent);
        } else if(hover_kind == 1 && hover_idx == i){
            yg_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, yg_mix(th->surface_alt, th->text, 28));
        }
        yui_text(&ui, 13, r, page_names[i], i == page ? th->accent_text : th->text, YUI_LEFT, i == page);
    }
    draw_page();
    yg_fill_rect(s, NAV_W, s->height - 26, s->width - NAV_W, 26, th->surface_alt);
    yg_fill_rect(s, NAV_W, s->height - 26, s->width - NAV_W, 1, th->border);
    yui_text(&ui, 12, yg_rect(NAV_W + 8, s->height - 25, s->width - NAV_W - 16, 25), status, th->text_muted, YUI_LEFT, 0);
    yw_window_damage_all(win);
}

/* ---------------- evenimente ---------------- */

static void goto_page(int p){
    if(p < 0 || p >= NPAGES){
        return;
    }
    page = p;
    hover_kind = 0;
    status[0] = 0;
    if(p == 1 && !dt_edited){
        dt_load_now();
    } else if(p == 2){
        net_load();
    } else if(p == 3){
        users_load();
    }
    dirty = 1;
}

static void hit(int x, int y, int *kind, int *idx){
    *kind = 0;
    *idx = -1;
    for(int i = 0; i < NPAGES; i++){
        if(yui_in(nav_rect(i), x, y)){
            *kind = 1;
            *idx = i;
            return;
        }
    }
    YgRect c = content_rect();
    if(page == 1){
        for(int i = 0; i < 6; i++){
            if(yui_in(dt_btn(i, 0), x, y)){
                *kind = 2;
                *idx = i * 2;
            } else if(yui_in(dt_btn(i, 1), x, y)){
                *kind = 2;
                *idx = i * 2 + 1;
            } else if(yui_in(dt_field_rect(i), x, y)){
                *kind = 3;
                *idx = i;
            }
        }
        for(int i = 0; i < 2; i++){
            if(yui_in(action_btn(i), x, y)){
                *kind = 2;
                *idx = 20 + i;
            }
        }
    } else if(page == 0){
        for(int i = 0; i < 2; i++){
            if(yui_in(theme_card(i), x, y)){
                *kind = 4;
                *idx = i;
            }
        }
    } else if(page == 2){
        int y0 = net_cards_end();
        if(yui_in(yg_rect(c.x + 20, y0 + 52, 170, 28), x, y)){
            *kind = 2;
            *idx = 30;
        } else if(yui_in(yg_rect(c.x + 200, y0 + 52, 130, 28), x, y)){
            *kind = 2;
            *idx = 31;
        }
    } else if(page == 3){
        for(int i = 0; i < nusers; i++){
            if(yui_in(yg_rect(c.x + 14, 76 + i * 26, c.w - 28, 26), x, y)){
                *kind = 2;
                *idx = 100 + i;
            }
        }
        if(yui_in(pw_btn(), x, y)){
            *kind = 2;
            *idx = 40;
        }
    }
}

static void on_mouse(const YwEvent *ev){
    int kind, idx;
    hit(ev->x, ev->y, &kind, &idx);
    if(ev->kind == YW_MOUSE_MOVE){
        if(kind != hover_kind || idx != hover_idx){
            hover_kind = kind;
            hover_idx = idx;
            dirty = 1;
        }
        return;
    }
    if(ev->kind != YW_MOUSE_PRESS || !(ev->buttons & YK_BTN_LEFT)){
        return;
    }
    if(kind == 1){
        goto_page(idx);
    } else if(kind == 3){
        dt_focus = idx;
        dirty = 1;
    } else if(kind == 4){
        apply_theme(idx);
    } else if(kind == 2){
        if(idx < 12){
            dt_focus = idx / 2;
            dt_step(idx / 2, (idx & 1) ? 1 : -1);
        } else if(idx == 20){
            dt_apply();
        } else if(idx == 21){
            dt_load_now();
            dt_edited = 0;
            set_status("Sincronizat cu ceasul sistemului");
        } else if(idx == 30){
            net_ping();
        } else if(idx == 31){
            net_load();
            ping_result[0] = 0;
            set_status("Reîmprospătat");
        } else if(idx == 40){
            change_password();
        } else if(idx >= 100 && idx < 100 + nusers){
            user_sel = idx - 100;
            dirty = 1;
        }
    }
}

static void on_key(const YwEvent *ev){
    if(!ev->pressed){
        return;
    }
    int k = ev->keycode, ctrl = (ev->modifiers & YK_MOD_CTRL) != 0;
    if(k >= K_F1 && k < K_F1 + NPAGES){
        goto_page(k - K_F1);
        return;
    }
    if(ctrl && k == K_PGUP){
        goto_page(page - 1);
        return;
    }
    if(ctrl && k == K_PGDN){
        goto_page(page + 1);
        return;
    }
    int a = ev->ascii;
    if(page == 0){
        int light = th == &yg_theme_light;
        if(k == K_LEFT){
            apply_theme(0);
        } else if(k == K_RIGHT){
            apply_theme(1);
        } else if(a == 't' || a == 'T'){
            apply_theme(!light);
        }
    } else if(page == 1){
        if(k == K_LEFT){
            dt_focus = (dt_focus + 5) % 6;
            dirty = 1;
        } else if(k == K_RIGHT || k == K_TAB){
            dt_focus = (dt_focus + 1) % 6;
            dirty = 1;
        } else if(k == K_UP){
            dt_step(dt_focus, 1);
        } else if(k == K_DOWN){
            dt_step(dt_focus, -1);
        } else if(k == K_PGUP){
            dt_step(dt_focus, 10);
        } else if(k == K_PGDN){
            dt_step(dt_focus, -10);
        } else if(k == K_ENTER){
            dt_apply();
        } else if(a == 'r' || a == 'R'){
            dt_load_now();
            dt_edited = 0;
            set_status("Sincronizat cu ceasul sistemului");
        }
    } else if(page == 2){
        if(a == 'r' || a == 'R'){
            net_load();
        } else if(a == 'p' || a == 'P'){
            net_ping();
        }
    } else if(page == 3){
        if(k == K_UP && user_sel > 0){
            user_sel--;
            dirty = 1;
        } else if(k == K_DOWN && user_sel + 1 < nusers){
            user_sel++;
            dirty = 1;
        } else if(k == K_ENTER){
            change_password();
        }
    }
}

static int usage(void){
    printf("uz: ysettings [-s socket] [-p pagina 1..5]\n");
    return 2;
}

int main(int argc, char **argv){
    const char *sock = NULL;
    int start = 0;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            usage();
            return 0;
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            sock = argv[++i];
        } else if(!strcmp(argv[i], "-p") && i + 1 < argc){
            start = atoi(argv[++i]) - 1;
            if(start < 0 || start >= NPAGES){
                return usage();
            }
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
        perror("ysettings: conectare");
        return 1;
    }
    int32_t q[6];
    th = yw_query(conn, YW_Q_THEME, 0, 0, q) == 0 && q[0] ? &yg_theme_light : &yg_theme_dark;
    ui.f = yg_font_load(YG_FONT_REGULAR);
    ui.b = yg_font_load(YG_FONT_BOLD);
    if(!ui.f){
        fprintf(stderr, "ysettings: fara font\n");
        return 1;
    }
    ui.t = th;
    win = yw_window_new(conn, 700, 430, "Setări");
    if(!win){
        perror("ysettings: fereastra");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    dt_load_now();
    users_load();
    goto_page(start);
    for(;;){
        if(dirty && !yw_pending(conn)){
            dirty = 0;
            draw_all();
        }
        int got = yw_pending(conn) > 0;
        if(!got){
            struct pollfd pf = {yw_fd(conn), POLLIN, 0};
            int r = poll(&pf, 1, page == 1 ? 1000 : 5000);
            if(r < 0 && errno != EINTR){
                break;
            }
            got = r > 0;
            if(!got){
                if(page == 1 && !dt_edited){
                    dt_load_now();
                }
                dirty = page == 1;
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
                dirty = 1;
            } else if(ev.type == YW_EV_THEME){
                th = ev.light ? &yg_theme_light : &yg_theme_dark;
                dirty = 1;
            }
        }
    }
    yw_window_destroy(win);
    yw_disconnect(conn);
    return 0;
}
