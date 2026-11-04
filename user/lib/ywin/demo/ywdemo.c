/* ywdemo (T10.2): fereastra demo pentru ycomp.
 *   ywdemo [-m clock|balls] [-t titlu] [-w latime] [-h inaltime] [-p perioada_ms] [-n cadre] [-s socket]
 * clock: ceasul, redesenat o data pe secunda (doar zona textului); balls: o bila care sare, la fiecare perioada.
 * Tastele apasate apar pe ultimul rand, un clic lasa un punct. Butonul de inchidere opreste programul. */
#include "ywin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/yinput.h>

static YgFont *font;
static const YgTheme *th = &yg_theme_dark;

static long long mono_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void draw_typed(YwWindow *w, const char *typed){
    YgSurface *s = yw_window_surface(w);
    YgRect r = yg_rect(0, s->height - 28, s->width, 28);
    yg_fill_rect(s, r.x, r.y, r.w, r.h, th->surface_alt);
    if(font){
        char line[80];
        snprintf(line, sizeof(line), "> %s", typed);
        yg_draw_text(s, font, 14, 8, r.y + 19, line, th->text);
    }
    yw_window_damage(w, r.x, r.y, r.w, r.h);
}

static void draw_clock(YwWindow *w, long tick){
    YgSurface *s = yw_window_surface(w);
    YgRect r = yg_rect(10, 20, s->width - 20, 60);
    yg_fill_rect(s, r.x, r.y, r.w, r.h, th->surface);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d  #%ld", tm->tm_hour, tm->tm_min, tm->tm_sec, tick);
    if(font){
        yg_draw_text_centered(s, font, 28, r, buf, th->text);
    }
    yw_window_damage(w, r.x, r.y, r.w, r.h);
}

int main(int argc, char **argv){
    const char *mode = "balls", *title = "ywdemo", *path = NULL;
    int ww = 320, wh = 220, period = 33;
    long frames = 0;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            printf("uz: ywdemo [-m clock|balls] [-t titlu] [-w latime] [-h inaltime] [-p ms] [-n cadre] [-s socket]\n");
            return 0;
        } else if(!strcmp(argv[i], "-m") && i + 1 < argc){
            mode = argv[++i];
        } else if(!strcmp(argv[i], "-t") && i + 1 < argc){
            title = argv[++i];
        } else if(!strcmp(argv[i], "-w") && i + 1 < argc){
            ww = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-h") && i + 1 < argc){
            wh = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-p") && i + 1 < argc){
            period = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-n") && i + 1 < argc){
            frames = atol(argv[++i]);
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            path = argv[++i];
        } else {
            fprintf(stderr, "ywdemo: optiune necunoscuta %s\n", argv[i]);
            return 2;
        }
    }
    int clock_mode = !strcmp(mode, "clock");
    if(clock_mode){
        period = 1000;
    }
    if(period < 1){
        period = 1;
    }
    YwConn *c = NULL;
    for(int i = 0; i < 50 && !c; i++){
        c = yw_connect(path);           /* compozitorul poate porni dupa noi */
        if(!c){
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
    }
    if(!c){
        perror("ywdemo: conectare");
        return 1;
    }
    font = yg_font_load(YG_FONT_REGULAR);
    YwWindow *w = yw_window_new(c, ww, wh, title);
    if(!w){
        perror("ywdemo: fereastra");
        return 1;
    }
    YgSurface *s = yw_window_surface(w);
    yg_clear(s, th->surface);
    char typed[48] = "";
    draw_typed(w, typed);
    yw_window_damage_all(w);

    float bx = 40, by = 40, vx = 3.2f, vy = 2.1f, br = 14;
    long tick = 0;
    long long next = mono_ms();
    int waiting_frame = 0;
    for(;;){
        long long now = mono_ms();
        int timeout = next > now ? (int)(next - now) : 0;
        if(waiting_frame && timeout < 1){
            timeout = 1000;             /* abia dupa YW_FRAME (sau din secunda in secunda) */
        }
        YwEvent ev;
        int r = yw_next_event(c, &ev, timeout);
        if(r < 0){
            break;
        }
        if(r > 0){
            if(ev.type == YW_EV_CLOSE){
                break;
            } else if(ev.type == YW_FRAME){
                waiting_frame = 0;
            } else if(ev.type == YW_EV_KEY && ev.pressed){
                size_t n = strlen(typed);
                if(ev.keycode == YK_KEY_BACKSPACE && n){
                    typed[n - 1] = 0;
                } else if(ev.ascii >= 32 && ev.ascii < 127 && n + 1 < sizeof(typed)){
                    typed[n] = (char)ev.ascii;
                    typed[n + 1] = 0;
                }
                draw_typed(w, typed);
            } else if(ev.type == YW_EV_MOUSE && ev.kind == YW_MOUSE_PRESS){
                yg_fill_circle(s, (float)ev.x, (float)ev.y, 4, th->accent);
                yw_window_damage(w, ev.x - 5, ev.y - 5, 11, 11);
            }
            continue;
        }
        if(waiting_frame){
            continue;
        }
        now = mono_ms();
        if(now < next){
            continue;
        }
        next += period;
        if(next < now){
            next = now + period;
        }
        tick++;
        if(clock_mode){
            draw_clock(w, tick);
        } else {
            int ox = (int)(bx - br) - 2, oy = (int)(by - br) - 2;
            yg_fill_rect(s, ox, oy, (int)(2 * br) + 5, (int)(2 * br) + 5, th->surface);
            bx += vx;
            by += vy;
            if(bx < br || bx > ww - br) vx = -vx;
            if(by < br || by > wh - 28 - br) vy = -vy;
            yg_fill_circle(s, bx, by, br, th->accent);
            int nx = (int)(bx - br) - 2, ny = (int)(by - br) - 2;
            int x0 = ox < nx ? ox : nx, y0 = oy < ny ? oy : ny;
            yw_window_damage(w, x0, y0, (int)(2 * br) + 10, (int)(2 * br) + 10);
        }
        waiting_frame = 1;
        if(frames && tick >= frames){
            break;
        }
    }
    yw_window_destroy(w);
    yw_disconnect(c);
    return 0;
}
