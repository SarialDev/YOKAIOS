#ifndef YWIN_H
#define YWIN_H
/* ywin (T10.2): biblioteca client a compozitorului ycomp. O conexiune (socket AF_UNIX), ferestre cu bufer in memorie
 * partajata (shm_open + mmap) peste care se deseneaza cu ygfx, apoi yw_window_damage. Evenimentele (taste, mouse,
 * focus, inchidere, cadre) vin cu yw_next_event; yw_fd se poate pune intr-un poll() propriu.
 * Legare: gcc ... -lywin -lygfx */
#include <stdint.h>
#include "ygfx.h"
#include "ywin_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct YwConn YwConn;
typedef struct YwWindow YwWindow;

typedef struct {
    int type;               /* YW_EV_*, YW_FRAME */
    uint32_t win;
    YwWindow *window;       /* fereastra (NULL daca nu mai exista) */
    int x, y;               /* mouse / enter: relativ la continut */
    int buttons, wheel, kind;   /* mouse: YK_BTN_*, roata, YW_MOUSE_* */
    int keycode, ascii, pressed, modifiers;     /* taste */
    int focus;              /* YW_EV_FOCUS */
    uint32_t frame;         /* YW_FRAME */
} YwEvent;

/* NULL = YW_SOCK_PATH. NULL la eroare (errno) */
YwConn *yw_connect(const char *path);
void yw_disconnect(YwConn *c);
int yw_fd(const YwConn *c);
int yw_screen_width(const YwConn *c);
int yw_screen_height(const YwConn *c);

/* Fereastra noua cu continut w x h (bufer zero = negru) si titlu; NULL la eroare (errno) */
YwWindow *yw_window_new(YwConn *c, int w, int h, const char *title);
void yw_window_destroy(YwWindow *w);
uint32_t yw_window_id(const YwWindow *w);
/* Suprafata ygfx peste bufer (desenul se vede dupa yw_window_damage) */
YgSurface *yw_window_surface(YwWindow *w);
/* Pozitia continutului pe ecran (la creare / dupa YW_MOVE) */
void yw_window_position(const YwWindow *w, int *x, int *y);
int yw_window_damage(YwWindow *w, int x, int y, int width, int height);
int yw_window_damage_all(YwWindow *w);
int yw_window_set_title(YwWindow *w, const char *title);
int yw_window_move(YwWindow *w, int x, int y);
/* Asteapta YW_FRAME pentru w (celelalte evenimente raman in coada); 0, sau -1 la eroare / timeout */
int yw_window_wait_frame(YwWindow *w, int timeout_ms);
/* Cate cadre YW_FRAME a primit w */
uint32_t yw_window_frames(const YwWindow *w);

/* Urmatorul eveniment: 1, 0 la expirare (timeout_ms < 0 = fara limita), -1 la eroare sau compozitor inchis */
int yw_next_event(YwConn *c, YwEvent *ev, int timeout_ms);
/* Evenimente deja primite si necitite */
int yw_pending(const YwConn *c);
/* YW_QUERY sincron; out[6] = v-urile raspunsului. 0 sau -1 */
int yw_query(YwConn *c, int kind, int a, int b, int32_t out[6]);

#ifdef __cplusplus
}
#endif
#endif
