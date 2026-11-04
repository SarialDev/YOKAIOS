#ifndef YWIN_PROTO_H
#define YWIN_PROTO_H
/* Protocolul client <-> compozitor ycomp (T10.2), pe un socket AF_UNIX SOCK_STREAM (implicit YW_SOCK_PATH).
 * Toate mesajele au exact 64 de octeti (struct yw_msg), in ambele sensuri. Buferul unei ferestre e un obiect
 * shm_open ("/ywin-<pid>-<n>") cu pixeli ARGB32 (alfa ignorat: continutul e opac), pe care clientul il deseneaza si
 * compozitorul il citeste; clientul anunta zonele schimbate cu YW_DAMAGE si primeste YW_FRAME dupa ce ele au
 * ajuns pe ecran (pentru ritm: un cadru nou abia dupa YW_FRAME). Coordonatele de fereastra sunt relative la continut. */
#include <stdint.h>

#define YW_SOCK_PATH     "/run/ycomp.sock"
#define YW_PROTO_VERSION 1

/* Decoratiunile desenate de compozitor: bara de titlu deasupra continutului, chenar de 1 pixel, buton de inchidere
 * in dreapta barei (la 6 pixeli de margine, centrat vertical) */
#define YW_TITLE_H       26
#define YW_BORDER        1
#define YW_CLOSE_SIZE    16

struct yw_msg {
    uint32_t type;
    uint32_t win;           /* id-ul ferestrei (0 = niciuna) */
    int32_t v[6];
    char s[32];             /* titlu / nume shm, terminat cu 0 */
};

/* client -> compozitor */
enum {
    YW_HELLO = 1,           /* v0 = versiune                         -> YW_WELCOME */
    YW_CREATE,              /* v0 = w, v1 = h, s = titlu             -> YW_CREATED sau YW_ERROR */
    YW_ATTACH,              /* win, s = nume shm, v0 = w, v1 = h, v2 = stride (pixeli), v3 = offset (octeti) -> YW_ATTACHED / YW_ERROR */
    YW_DAMAGE,              /* win, v0..v3 = x, y, w, h              -> (dupa compunere) YW_FRAME */
    YW_TITLE,               /* win, s */
    YW_MOVE,                /* win, v0 = x, v1 = y (coltul ferestrei pe ecran) */
    YW_DESTROY,             /* win */
    YW_QUERY,               /* v0 = YW_Q_*, v1, v2                   -> YW_REPLY */

    /* compozitor -> client */
    YW_WELCOME = 64,        /* v0 = latime ecran, v1 = inaltime, v2 = id client */
    YW_CREATED,             /* win, v0 = x, v1 = y (continutul pe ecran) */
    YW_ATTACHED,            /* win: buferul e mapat de compozitor (clientul poate face shm_unlink) */
    YW_ERROR,               /* v0 = errno, v1 = tipul cererii */
    YW_REPLY,               /* v[] dupa YW_Q_* */
    YW_FRAME,               /* win, v0 = numarul cadrului compus */
    YW_EV_KEY,              /* win, v0 = keycode, v1 = ascii, v2 = apasat, v3 = modificatori */
    YW_EV_MOUSE,            /* win, v0 = x, v1 = y, v2 = butoane, v3 = roata, v4 = YW_MOUSE_* */
    YW_EV_ENTER,            /* win, v0 = x, v1 = y */
    YW_EV_LEAVE,            /* win */
    YW_EV_FOCUS,            /* win, v0 = 1 primit / 0 pierdut */
    YW_EV_CLOSE,            /* win: utilizatorul a apasat butonul de inchidere */
    YW_EV_RESIZE,           /* win, v0 = latime, v1 = inaltime: dimensiunea noua a continutului (T10.3). Clientul face un
                               bufer nou si il trimite cu YW_ATTACH (libywin o face singura in yw_next_event); pana atunci
                               compozitorul arata buferul vechi taiat / completat cu culoarea suprafetei */
};

enum { YW_MOUSE_MOVE = 0, YW_MOUSE_PRESS = 1, YW_MOUSE_RELEASE = 2, YW_MOUSE_WHEEL = 3 };

/* YW_QUERY (introspectie, pentru teste si unelte) */
enum {
    YW_Q_STATS = 1,         /* v0 = cadre compuse, v1 = treziri ale buclei, v2 = FPS tinta, v3 = animatii (1/0),
                               v4 = ferestre, v5 = fereastra cu focus */
    YW_Q_STATS2,            /* v0, v1 = cursor x, y; v2 = incarcarea RT (ppm); v3 = de cate ori s-a redus FPS-ul;
                               v4 = pixeli compusi (mii); v5 = politica de planificare a compozitorului (SCHED_*) */
    YW_Q_WIN,               /* v1 = id: v0 = x, v1 = y (continut), v2 = w, v3 = h, v4 = YW_DAMAGE primite, v5 = YW_FRAME trimise */
    YW_Q_PIXEL,             /* v1 = x, v2 = y: v0 = pixelul din back buffer (0xRRGGBB) */
    YW_Q_WM,                /* managerul de ferestre (T10.3). v1 = 0: v0 = spatiul curent, v1 = masca modurilor (bit i = tiling pe
                               spatiul i), v2 = ferestre vizibile, v3 = aranjari facute, v4 = id-ul ferestrei cu focus, v5 =
                               schimbari de spatiu. v1 = id fereastra: v0 = spatiul, v1 = bit 0 minimizata | snap << 4 (0 niciunul,
                               1 stanga, 2 dreapta, 3 maxim, 4..7 colturi TL TR BL BR) | vizibila << 8, v2 = YW_EV_RESIZE trimise,
                               v3, v4 = dimensiunea buferului atasat, v5 = pozitia in z-order (0 = jos); v0 = -1 daca nu exista */
};

#endif
