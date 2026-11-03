#ifndef YGFX_H
#define YGFX_H
/* ygfx (T10.1): biblioteca grafica user a YokaiOS, numai CPU (fara GPU).
 *   - YgSurface: pixeli ARGB32 (0xAARRGGBB, alfa ne-premultiplicat), stride in pixeli, dreptunghi de clipping
 *   - primitive: fill / rect / linie / dreptunghi rotunjit / cerc / poligon (antialias prin acoperire exacta de arie)
 *   - blit simplu si cu alfa (alfa per pixel * opacitate), toate decupate la clip
 *   - text TrueType prin stb_truetype, cu cache LRU de glife (bitmap-uri de acoperire) per font si marime
 *   - teme (structuri de culori): yg_theme_dark, yg_theme_light; logo Yokai vectorial
 *   - /dev/fb0: yg_fb_open mapeaza ecranul (mmap MAP_SHARED), yg_fb_present copiaza o suprafata pe ecran
 * Coordonatele sunt in pixeli, originea sus-stanga; dreptunghiurile sunt [x, x + w) x [y, y + h).
 * Antet + libygfx.a in sysroot: gcc ... -lygfx (ordinea: -lygfx inainte de libc, o pune spec-ul). */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t YgColor;

#define YG_ARGB(a, r, g, b) ((YgColor)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define YG_RGB(r, g, b)     YG_ARGB(255, r, g, b)
#define YG_A(c) ((uint8_t)((c) >> 24))
#define YG_R(c) ((uint8_t)((c) >> 16))
#define YG_G(c) ((uint8_t)((c) >> 8))
#define YG_B(c) ((uint8_t)(c))
/* Aceeasi culoare cu alt alfa */
#define YG_WITH_ALPHA(c, a) (((YgColor)(c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

typedef struct {
    int x, y, w, h;
} YgRect;

typedef struct {
    float x, y;
} YgPoint;

typedef struct {
    uint32_t *pixels;
    int width, height;
    int stride;             /* pixeli pe linie (>= width) */
    YgRect clip;            /* mereu in interiorul suprafetei */
    int owned;              /* 1 = pixels alocat de yg_surface_new */
} YgSurface;

/* ---- dreptunghiuri ---- */
YgRect yg_rect(int x, int y, int w, int h);
YgRect yg_rect_intersect(YgRect a, YgRect b);   /* w = h = 0 daca nu se intersecteaza */
int yg_rect_empty(YgRect r);
int yg_rect_contains(YgRect r, int x, int y);

/* ---- suprafete ---- */
/* Suprafata noua w x h, transparenta (0x00000000); NULL fara memorie */
YgSurface *yg_surface_new(int w, int h);
/* Suprafata peste pixeli existenti (ex. memoria video); nu ii elibereaza */
void yg_surface_wrap(YgSurface *s, uint32_t *pixels, int w, int h, int stride);
void yg_surface_free(YgSurface *s);
/* Clip = r intersectat cu suprafata; intoarce clip-ul vechi (pentru restaurare) */
YgRect yg_set_clip(YgSurface *s, YgRect r);
void yg_reset_clip(YgSurface *s);

/* ---- pixeli si culori ---- */
/* src peste dst (Porter-Duff "over", ne-premultiplicat) */
YgColor yg_blend(YgColor dst, YgColor src);
/* Interpolare liniara a -> b cu t in [0, 255] */
YgColor yg_mix(YgColor a, YgColor b, uint8_t t);
YgColor yg_get_pixel(const YgSurface *s, int x, int y);     /* 0 in afara suprafetei */
void yg_set_pixel(YgSurface *s, int x, int y, YgColor c);   /* scrie, fara amestec (respecta clip) */
void yg_blend_pixel(YgSurface *s, int x, int y, YgColor c); /* amesteca (respecta clip) */

/* ---- primitive (toate amesteca daca alfa < 255 si respecta clip-ul) ---- */
void yg_clear(YgSurface *s, YgColor c);                         /* tot clip-ul, scris direct (fara amestec) */
void yg_fill_rect(YgSurface *s, int x, int y, int w, int h, YgColor c);
void yg_draw_rect(YgSurface *s, int x, int y, int w, int h, int thickness, YgColor c);
/* Linie de 1 pixel (Bresenham), capetele incluse */
void yg_draw_line(YgSurface *s, int x0, int y0, int x1, int y1, YgColor c);
/* Linie groasa cu antialias (capete drepte) */
void yg_draw_line_aa(YgSurface *s, float x0, float y0, float x1, float y1, float width, YgColor c);
/* Gradient vertical top -> bottom */
void yg_fill_gradient_v(YgSurface *s, int x, int y, int w, int h, YgColor top, YgColor bottom);
/* Dreptunghi rotunjit: colturi cu antialias, raza limitata la min(w, h) / 2 */
void yg_fill_rounded_rect(YgSurface *s, int x, int y, int w, int h, int radius, YgColor c);
void yg_draw_rounded_rect(YgSurface *s, int x, int y, int w, int h, int radius, int thickness, YgColor c);
void yg_fill_circle(YgSurface *s, float cx, float cy, float r, YgColor c);
void yg_draw_circle(YgSurface *s, float cx, float cy, float r, float thickness, YgColor c);
/* Poligon (sau mai multe contururi: counts[i] puncte fiecare) umplut cu antialias, regula nonzero:
 * un contur in sens invers in interiorul altuia face o gaura */
void yg_fill_polygon(YgSurface *s, const YgPoint *pts, int n, YgColor c);
void yg_fill_path(YgSurface *s, const YgPoint *pts, const int *counts, int ncontours, YgColor c);
/* Masca de acoperire (0..255) w x h colorata cu c (alfa(c) * masca), la (x, y) */
void yg_fill_mask(YgSurface *s, int x, int y, const uint8_t *mask, int w, int h, int mask_stride, YgColor c);

/* ---- blit ---- */
/* Copiaza src_rect (NULL = toata sursa) din src la (dx, dy), fara amestec */
void yg_blit(YgSurface *dst, int dx, int dy, const YgSurface *src, const YgRect *src_rect);
/* Ca yg_blit, dar sursa se amesteca peste destinatie cu alfa ei * opacity / 255 */
void yg_blit_alpha(YgSurface *dst, int dx, int dy, const YgSurface *src, const YgRect *src_rect, uint8_t opacity);

/* ---- text (stb_truetype) ---- */
typedef struct YgFont YgFont;

typedef struct {
    uint64_t hits, misses, evictions;
    uint32_t glyphs;            /* glife in cache acum */
    uint32_t bytes;             /* memoria bitmap-urilor din cache */
    uint32_t max_glyphs, max_bytes;
} YgCacheStats;

#define YG_FONT_DIR      "/usr/share/fonts"
#define YG_FONT_REGULAR  YG_FONT_DIR "/DejaVuSans.ttf"
#define YG_FONT_BOLD     YG_FONT_DIR "/DejaVuSans-Bold.ttf"

/* Citeste fisierul TTF in memorie; NULL daca lipseste sau nu e un font valid */
YgFont *yg_font_load(const char *path);
/* Font dintr-un buffer existent (ramane al apelantului, trebuie sa traiasca cat fontul) */
YgFont *yg_font_from_memory(const uint8_t *data, size_t len);
void yg_font_free(YgFont *f);
/* Metrici verticale la marimea px (inaltimea in pixeli de la descendent la ascendent): ascent > 0, descent < 0 */
void yg_font_vmetrics(YgFont *f, float px, int *ascent, int *descent, int *line_gap);
/* Latimea in pixeli a textului UTF-8 (cu kerning) */
int yg_text_width(YgFont *f, float px, const char *utf8);
/* Deseneaza textul UTF-8 cu linia de baza la y; '\n' trece la linia urmatoare. Intoarce latimea ultimei linii */
int yg_draw_text(YgSurface *s, YgFont *f, float px, int x, int y, const char *utf8, YgColor c);
/* Textul centrat (orizontal si vertical, dupa ascent/descent) in r */
void yg_draw_text_centered(YgSurface *s, YgFont *f, float px, YgRect r, const char *utf8, YgColor c);
/* Cache-ul de glife (implicit 512 glife / 1 MiB); limitele noi golesc ce e peste ele */
void yg_font_cache_limit(YgFont *f, uint32_t max_glyphs, uint32_t max_bytes);
void yg_font_cache_clear(YgFont *f);
YgCacheStats yg_font_cache_stats(const YgFont *f);

/* ---- teme ---- */
typedef struct {
    const char *name;
    YgColor background;     /* fundalul ecranului */
    YgColor surface;        /* panouri */
    YgColor surface_alt;    /* panouri in panouri, campuri */
    YgColor border;
    YgColor text;
    YgColor text_muted;
    YgColor accent;         /* butoane, selectie */
    YgColor accent_text;    /* text pe accent */
    YgColor logo;           /* logo-ul Yokai */
    YgColor shadow;         /* umbra (cu alfa) */
} YgTheme;

/* Dark: fundal negru, text alb, accent/logo rosu. Light: fundal alb, text negru, logo (si accent) verde inchis */
extern const YgTheme yg_theme_dark;
extern const YgTheme yg_theme_light;
/* Dupa nume ("dark" / "light"), NULL daca nu exista */
const YgTheme *yg_theme_by_name(const char *name);

/* ---- logo ---- */
/* Masca Yokai (oni cu doua coarne) cu centrul in (cx, cy), incadrata intr-un patrat size x size. Ochii si gura
 * se decupeaza (raman culoarea de dedesubt) */
void yg_draw_logo(YgSurface *s, float cx, float cy, float size, YgColor c);

/* ---- framebuffer (/dev/fb0) ---- */
typedef struct {
    int fd;
    uint8_t *map;           /* memoria video mapata */
    size_t map_len;
    int width, height;
    int pitch;              /* bytes pe linie */
    uint8_t shift[3], bits[3];  /* R, G, B */
    int native;             /* 1 = format xRGB 8:8:8 (copiere directa de linii) */
} YgFb;

/* Deschide si mapeaza dispozitivul (NULL = /dev/fb0). 0 sau -errno. Cat timp e mapat, consola nu scrie pe ecran */
int yg_fb_open(YgFb *fb, const char *path);
/* Demapeaza si inchide: ecranul consolei revine */
void yg_fb_close(YgFb *fb);
/* Copiaza src (aceeasi marime sau mai mica, la 0,0) pe ecran; area = NULL pentru toata suprafata */
void yg_fb_present(YgFb *fb, const YgSurface *src, const YgRect *area);
/* Daca fb e nativ: suprafata direct peste memoria video (desen direct, fara back buffer); altfel -1 */
int yg_fb_surface(YgFb *fb, YgSurface *out);

#ifdef __cplusplus
}
#endif
#endif
