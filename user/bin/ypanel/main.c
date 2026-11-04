/* ypanel (T10.4): taskbar + meniul Start pentru ycomp.
 *   ypanel [-s socket] [-c fisier_config] [-t dark|light]
 * Se conecteaza la compozitor, isi cere o fereastra cu rolul PANEL (jos, 40 px, rezerva zona de lucru) si primeste de la el
 * lista ferestrelor (YW_EV_TASK) si tasta Win (YW_EV_START). Ceasul se reface la minut; in rest doarme in poll
 * (se trezeste cel mult din cateva secunde ca sa culeaga procesele terminate). */
#include "panel.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/yinput.h>

Panel P;

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig){
    if(sig != SIGCHLD){
        stop_flag = 1;
    }
}

static void task_event(const YwEvent *ev){
    P.events_task++;
    if(ev->task == 0){                      /* starea spatiilor de lucru */
        P.tilemask = ev->task_flags;
        P.ws = ev->task_ws;
        return;
    }
    int at = -1;
    for(int i = 0; i < P.ntasks; i++){
        if(P.tasks[i].id == ev->task){
            at = i;
            break;
        }
    }
    if(!(ev->task_flags & YW_TF_EXISTS)){
        if(at >= 0){
            memmove(&P.tasks[at], &P.tasks[at + 1], (size_t)(P.ntasks - at - 1) * sizeof(Task));
            P.ntasks--;
        }
        return;
    }
    if(at < 0){
        if(P.ntasks >= MAX_TASKS){
            return;
        }
        at = P.ntasks++;
    }
    Task *k = &P.tasks[at];
    k->id = ev->task;
    k->flags = ev->task_flags;
    k->ws = ev->task_ws;
    snprintf(k->title, sizeof(k->title), "%s", ev->title);
}

static void update_hover(int x, int y){
    int kind, idx;
    bar_hit(x, y, &kind, &idx);
    if(kind != P.hover_kind || idx != P.hover_idx){
        P.hover_kind = kind;
        P.hover_idx = idx;
        bar_redraw();
    }
}

static void bar_mouse(const YwEvent *ev){
    if(ev->kind == YW_MOUSE_MOVE){
        update_hover(ev->x, ev->y);
        return;
    }
    if(ev->kind != YW_MOUSE_PRESS){
        return;
    }
    int kind, idx;
    bar_hit(ev->x, ev->y, &kind, &idx);
    if(kind == H_START){
        if(ev->buttons & YK_BTN_LEFT){
            menu_toggle();
        }
        return;
    }
    menu_close();                           /* orice alt clic pe bara inchide meniul */
    if(kind == H_PIN && (ev->buttons & YK_BTN_LEFT)){
        app_launch(P.apps[idx].cmd);
    } else if(kind == H_TASK){
        if(ev->buttons & YK_BTN_LEFT){
            yw_task_action(P.c, P.tasks[idx].id, YW_TA_TOGGLE);
        } else if(ev->buttons & YK_BTN_RIGHT){
            yw_task_action(P.c, P.tasks[idx].id, YW_TA_CLOSE);
        }
    }
}

static int usage(void){
    printf("uz: ypanel [-s socket] [-c fisier_config] [-t dark|light]\n");
    return 2;
}

int main(int argc, char **argv){
    const char *sock = NULL, *conf = CONF_PATH, *theme = NULL;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            usage();
            return 0;
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            sock = argv[++i];
        } else if(!strcmp(argv[i], "-c") && i + 1 < argc){
            conf = argv[++i];
        } else if(!strcmp(argv[i], "-t") && i + 1 < argc){
            theme = argv[++i];
        } else {
            return usage();
        }
    }
    for(int i = 0; i < 50 && !P.c; i++){
        P.c = yw_connect(sock);             /* compozitorul poate porni dupa noi */
        if(!P.c){
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
    }
    if(!P.c){
        perror("ypanel: conectare");
        return 1;
    }
    P.sw = yw_screen_width(P.c);
    P.sh = yw_screen_height(P.c);
    P.th = theme ? yg_theme_by_name(theme) : NULL;
    if(!P.th){
        int32_t q[6];
        P.th = yw_query(P.c, YW_Q_THEME, 0, 0, q) == 0 && q[0] ? &yg_theme_light : &yg_theme_dark;
    }
    P.font = yg_font_load(YG_FONT_REGULAR);
    P.bold = yg_font_load(YG_FONT_BOLD);
    conf_load(conf);
    P.win = yw_window_new_role(P.c, P.sw, PANEL_H, "ypanel", YW_ROLE_PANEL, 0, 0);
    if(!P.win){
        perror("ypanel: panou");
        return 1;
    }
    P.win_id = yw_window_id(P.win);
    P.hover_idx = -1;
    bar_update_clock();
    bar_redraw();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);          /* trezeste poll-ul ca sa culegem procesul terminat */

    int dirty = 0;
    while(!stop_flag){
        if(!yw_pending(P.c)){
            time_t now = time(NULL);
            int wait = (60 - (int)(now % 60)) * 1000 + 30;
            if(wait > 4000){
                wait = 4000;
            }
            struct pollfd pf = {yw_fd(P.c), POLLIN, 0};
            int pr = poll(&pf, 1, dirty ? 0 : wait);
            if(pr < 0 && errno != EINTR){
                break;
            }
            if(pr > 0 || yw_pending(P.c)){
                /* evenimente: mai jos */
            } else {
                if(bar_update_clock()){
                    dirty = 1;
                }
                if(dirty){
                    dirty = 0;
                    bar_redraw();
                }
                reap_children();
                continue;
            }
        }
        YwEvent ev;
        int r = yw_next_event(P.c, &ev, 0);
        if(r < 0){
            break;                          /* compozitorul s-a inchis */
        }
        if(r > 0){
            if(ev.type == YW_EV_TASK){
                task_event(&ev);
                dirty = 1;
            } else if(ev.type == YW_EV_START){
                P.events_start++;
                menu_toggle();
            } else if(ev.win == P.win_id){
                if(ev.type == YW_EV_MOUSE){
                    bar_mouse(&ev);
                } else if(ev.type == YW_EV_LEAVE){
                    update_hover(-1, -1);
                }
            } else if(menu_is_open() && ev.win == menu_window_id()){
                menu_event(&ev);
            }
            if(yw_pending(P.c)){
                continue;                   /* mai intai tot ce a venit odata */
            }
        }
        if(bar_update_clock()){
            dirty = 1;
        }
        if(dirty){
            dirty = 0;
            bar_redraw();
        }
        reap_children();
    }
    menu_close();
    yw_window_destroy(P.win);
    yw_disconnect(P.c);
    return 0;
}
