/* ygfxtest (T10.1): verificari pentru ygfx (suprafete in RAM: primitive, clipping, alfa, blit, poligoane cu antialias,
 * text si cache-ul de glife, teme, logo) si pentru /dev/fb0 (ioctl FBIOGET_*, mmap MAP_SHARED, read() vede ce s-a
 * scris prin mapare, fork pastreaza maparea comuna, munmap reda ecranul). Rulat de test=ygfx; iese cu 0 daca totul trece. */
#include "ygfx.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/fb.h>

static int checks, fails;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)){ \
        fails++; \
        printf("ygfxtest: FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

static int near(int a, int b, int tol){
    return a - b <= tol && b - a <= tol;
}

static int near_color(YgColor a, YgColor b, int tol){
    return near(YG_A(a), YG_A(b), tol) && near(YG_R(a), YG_R(b), tol) && near(YG_G(a), YG_G(b), tol) &&
           near(YG_B(a), YG_B(b), tol);
}

static long count_color(const YgSurface *s, YgColor c){
    long n = 0;
    for(int y = 0; y < s->height; y++){
        for(int x = 0; x < s->width; x++){
            n += yg_get_pixel(s, x, y) == c;
        }
    }
    return n;
}

/* Suma acoperirii (canalul rosu / 255) dupa o umplere alba pe negru */
static double coverage(const YgSurface *s){
    double sum = 0.0;
    for(int y = 0; y < s->height; y++){
        for(int x = 0; x < s->width; x++){
            sum += YG_R(yg_get_pixel(s, x, y)) / 255.0;
        }
    }
    return sum;
}

static void test_rects(void){
    YgRect a = yg_rect(0, 0, 10, 10), b = yg_rect(5, 5, 10, 10);
    YgRect i = yg_rect_intersect(a, b);
    CHECK(i.x == 5 && i.y == 5 && i.w == 5 && i.h == 5, "intersectie %d,%d %dx%d", i.x, i.y, i.w, i.h);
    CHECK(yg_rect_empty(yg_rect_intersect(a, yg_rect(20, 20, 5, 5))), "fara intersectie");
    CHECK(yg_rect_contains(a, 9, 9) && !yg_rect_contains(a, 10, 9), "contains");
}

static void test_surface_clip(void){
    YgSurface *s = yg_surface_new(64, 48);
    CHECK(s && s->stride == 64 && yg_get_pixel(s, 3, 3) == 0, "suprafata noua transparenta");
    if(!s) return;
    yg_clear(s, YG_RGB(10, 20, 30));
    CHECK(count_color(s, YG_RGB(10, 20, 30)) == 64 * 48, "clear umple tot");
    YgRect old = yg_set_clip(s, yg_rect(10, 10, 20, 15));
    CHECK(old.w == 64 && old.h == 48, "clip vechi intors");
    yg_fill_rect(s, -100, -100, 1000, 1000, YG_RGB(255, 0, 0));
    CHECK(count_color(s, YG_RGB(255, 0, 0)) == 20 * 15, "fill decupat la clip: %ld", count_color(s, YG_RGB(255, 0, 0)));
    CHECK(yg_get_pixel(s, 9, 10) == YG_RGB(10, 20, 30) && yg_get_pixel(s, 10, 10) == YG_RGB(255, 0, 0) &&
          yg_get_pixel(s, 29, 24) == YG_RGB(255, 0, 0) && yg_get_pixel(s, 30, 24) == YG_RGB(10, 20, 30), "marginile clip-ului");
    yg_draw_line(s, 0, 0, 63, 47, YG_RGB(0, 255, 0));
    for(int y = 0; y < 48; y++){
        for(int x = 0; x < 64; x++){
            if(yg_get_pixel(s, x, y) == YG_RGB(0, 255, 0) && !yg_rect_contains(s->clip, x, y)){
                CHECK(0, "linie in afara clip-ului la %d,%d", x, y);
                y = 48;
                break;
            }
        }
    }
    yg_set_clip(s, yg_rect(50, 40, 100, 100));
    CHECK(s->clip.x == 50 && s->clip.w == 14 && s->clip.h == 8, "clip taiat la suprafata");
    yg_reset_clip(s);
    CHECK(s->clip.w == 64 && s->clip.h == 48, "reset clip");
    yg_set_pixel(s, 70, 3, YG_RGB(1, 1, 1));            /* in afara: ignorat */
    CHECK(yg_get_pixel(s, 70, 3) == 0, "get in afara = 0");
    yg_surface_free(s);

    /* wrap peste un buffer cu stride mai mare */
    uint32_t buf[8 * 4];
    memset(buf, 0, sizeof(buf));
    YgSurface w;
    yg_surface_wrap(&w, buf, 5, 4, 8);
    yg_clear(&w, 0xFFFFFFFF);
    CHECK(buf[4] == 0xFFFFFFFF && buf[5] == 0 && buf[8 * 3 + 4] == 0xFFFFFFFF && buf[8 * 3 + 7] == 0, "stride respectat");
    yg_surface_free(&w);                                /* nu e al ei: nu elibereaza */
}

static void test_blend(void){
    CHECK(yg_blend(YG_RGB(255, 255, 255), YG_ARGB(0, 255, 0, 0)) == YG_RGB(255, 255, 255), "alfa 0 nu schimba");
    CHECK(yg_blend(YG_RGB(255, 255, 255), YG_RGB(1, 2, 3)) == YG_RGB(1, 2, 3), "alfa 255 inlocuieste");
    YgColor h = yg_blend(YG_RGB(255, 255, 255), YG_ARGB(128, 255, 0, 0));
    CHECK(near_color(h, YG_RGB(255, 127, 127), 1), "50%% rosu peste alb = %08x", h);
    YgColor t = yg_blend(0, YG_ARGB(100, 10, 20, 30));
    CHECK(t == YG_ARGB(100, 10, 20, 30), "peste transparent ramane sursa: %08x", t);
    YgColor u = yg_blend(YG_ARGB(128, 0, 0, 255), YG_ARGB(128, 255, 0, 0));
    CHECK(near(YG_A(u), 192, 1) && near(YG_R(u), 170, 2) && near(YG_B(u), 85, 2), "peste semi-transparent: %08x", u);
    CHECK(yg_mix(YG_RGB(0, 0, 0), YG_RGB(255, 255, 255), 255) == YG_RGB(255, 255, 255), "mix 255");
    CHECK(near_color(yg_mix(YG_RGB(0, 0, 0), YG_RGB(200, 100, 50), 128), YG_RGB(100, 50, 25), 1), "mix 128");

    YgSurface *s = yg_surface_new(10, 10);
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_fill_rect(s, 0, 0, 10, 10, YG_ARGB(64, 255, 255, 255));
    yg_fill_rect(s, 0, 0, 10, 10, YG_ARGB(64, 255, 255, 255));
    YgColor p = yg_get_pixel(s, 5, 5);
    CHECK(near(YG_R(p), 112, 2) && YG_A(p) == 255, "doua straturi 25%%: %08x", p);
    yg_surface_free(s);
}

static void test_primitives(void){
    YgSurface *s = yg_surface_new(120, 80);
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_line(s, 0, 0, 100, 0, YG_RGB(255, 255, 255));
    CHECK(count_color(s, YG_RGB(255, 255, 255)) == 101, "linie orizontala 101 pixeli");
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_line(s, 70, 70, 10, 10, YG_RGB(255, 255, 255));
    CHECK(count_color(s, YG_RGB(255, 255, 255)) == 61 && yg_get_pixel(s, 40, 40) == YG_RGB(255, 255, 255),
          "diagonala inversa 61 pixeli");
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_line(s, 3, 5, 50, 17, YG_RGB(255, 255, 255));
    CHECK(yg_get_pixel(s, 3, 5) == YG_RGB(255, 255, 255) && yg_get_pixel(s, 50, 17) == YG_RGB(255, 255, 255) &&
          count_color(s, YG_RGB(255, 255, 255)) == 48, "linie oblica: capete si 48 pixeli");

    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_rect(s, 10, 10, 50, 30, 3, YG_RGB(0, 255, 0));
    CHECK(count_color(s, YG_RGB(0, 255, 0)) == 50 * 30 - 44 * 24, "contur dreptunghi grosime 3");
    CHECK(yg_get_pixel(s, 30, 25) == YG_RGB(0, 0, 0) && yg_get_pixel(s, 12, 12) == YG_RGB(0, 255, 0), "interior gol");

    yg_clear(s, YG_RGB(0, 0, 0));
    yg_fill_rounded_rect(s, 10, 10, 80, 50, 12, YG_RGB(255, 255, 255));
    CHECK(yg_get_pixel(s, 10, 10) == YG_RGB(0, 0, 0) && yg_get_pixel(s, 89, 59) == YG_RGB(0, 0, 0), "colturi rotunjite goale");
    CHECK(yg_get_pixel(s, 50, 10) == YG_RGB(255, 255, 255) && yg_get_pixel(s, 10, 35) == YG_RGB(255, 255, 255) &&
          yg_get_pixel(s, 50, 35) == YG_RGB(255, 255, 255), "marginile drepte si centrul pline");
    double area = 80.0 * 50.0 - (4.0 - 3.14159265) * 144.0;
    double cov = coverage(s);
    CHECK(cov > area - 4 && cov < area + 4, "aria dreptunghiului rotunjit %.1f (asteptat %.1f)", cov, area);
    YgColor edge = yg_get_pixel(s, 13, 13);
    CHECK(YG_R(edge) > 0 && YG_R(edge) < 255, "antialias pe arc: %08x", edge);

    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_rounded_rect(s, 10, 10, 80, 50, 12, 4, YG_RGB(255, 255, 255));
    CHECK(yg_get_pixel(s, 50, 35) == YG_RGB(0, 0, 0) && yg_get_pixel(s, 50, 11) == YG_RGB(255, 255, 255) &&
          yg_get_pixel(s, 50, 14) == YG_RGB(0, 0, 0), "contur rotunjit: gol in interior");

    yg_clear(s, YG_RGB(0, 0, 0));
    yg_fill_gradient_v(s, 0, 0, 10, 80, YG_RGB(0, 0, 0), YG_RGB(255, 255, 255));
    CHECK(yg_get_pixel(s, 5, 0) == YG_RGB(0, 0, 0) && yg_get_pixel(s, 5, 79) == YG_RGB(255, 255, 255) &&
          near(YG_R(yg_get_pixel(s, 5, 40)), 129, 2), "gradient vertical");
    yg_surface_free(s);
}

static void test_polygons(void){
    YgSurface *s = yg_surface_new(100, 100);
    yg_clear(s, YG_RGB(0, 0, 0));
    YgPoint tri[3] = {{10.0f, 10.0f}, {90.0f, 10.0f}, {10.0f, 70.0f}};
    yg_fill_polygon(s, tri, 3, YG_RGB(255, 255, 255));
    double cov = coverage(s);
    CHECK(cov > 2400.0 - 3 && cov < 2400.0 + 3, "aria triunghiului %.2f (2400)", cov);
    CHECK(yg_get_pixel(s, 20, 20) == YG_RGB(255, 255, 255) && yg_get_pixel(s, 80, 60) == YG_RGB(0, 0, 0), "interior / exterior");

    /* acelasi triunghi in sens invers: aceeasi arie */
    yg_clear(s, YG_RGB(0, 0, 0));
    YgPoint rev[3] = {tri[2], tri[1], tri[0]};
    yg_fill_polygon(s, rev, 3, YG_RGB(255, 255, 255));
    CHECK(near((int)coverage(s), 2400, 3), "sens invers");

    yg_clear(s, YG_RGB(0, 0, 0));
    yg_fill_circle(s, 50.0f, 50.0f, 30.0f, YG_RGB(255, 255, 255));
    cov = coverage(s);
    double disc = 3.14159265 * 900.0;
    CHECK(cov > disc * 0.99 && cov < disc * 1.005, "aria discului %.1f (%.1f)", cov, disc);

    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_circle(s, 50.0f, 50.0f, 30.0f, 6.0f, YG_RGB(255, 255, 255));
    double ring = 3.14159265 * (33.0 * 33.0 - 27.0 * 27.0);
    cov = coverage(s);
    CHECK(cov > ring * 0.98 && cov < ring * 1.01 && yg_get_pixel(s, 50, 50) == YG_RGB(0, 0, 0),
          "inel %.1f (%.1f), gaura in centru", cov, ring);

    /* poligon in mare parte in afara suprafetei / clip-ului */
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_set_clip(s, yg_rect(20, 20, 30, 30));
    YgPoint big[4] = {{-500.0f, -500.0f}, {500.0f, -500.0f}, {500.0f, 500.0f}, {-500.0f, 500.0f}};
    yg_fill_polygon(s, big, 4, YG_RGB(255, 255, 255));
    CHECK(count_color(s, YG_RGB(255, 255, 255)) == 900, "poligon urias decupat la clip");
    yg_reset_clip(s);

    /* linie groasa cu antialias: aria = lungime * grosime */
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_draw_line_aa(s, 10.0f, 10.0f, 70.0f, 90.0f, 4.0f, YG_RGB(255, 255, 255));
    CHECK(near((int)coverage(s), 400, 3), "linie AA 100 x 4 = %.1f", coverage(s));
    yg_surface_free(s);
}

static void test_blit(void){
    YgSurface *dst = yg_surface_new(40, 40), *src = yg_surface_new(10, 10);
    yg_clear(dst, YG_RGB(0, 0, 255));
    yg_clear(src, YG_RGB(255, 0, 0));
    src->pixels[0] = YG_ARGB(0, 0, 255, 0);
    yg_blit(dst, 5, 5, src, NULL);
    CHECK(yg_get_pixel(dst, 5, 5) == YG_ARGB(0, 0, 255, 0) && yg_get_pixel(dst, 14, 14) == YG_RGB(255, 0, 0) &&
          yg_get_pixel(dst, 15, 15) == YG_RGB(0, 0, 255), "blit copiaza (si alfa)");
    yg_clear(dst, YG_RGB(0, 0, 255));
    yg_blit_alpha(dst, 5, 5, src, NULL, 255);
    CHECK(yg_get_pixel(dst, 5, 5) == YG_RGB(0, 0, 255) && yg_get_pixel(dst, 6, 5) == YG_RGB(255, 0, 0), "blit_alpha");
    yg_clear(dst, YG_RGB(0, 0, 255));
    yg_blit_alpha(dst, 5, 5, src, NULL, 128);
    CHECK(near_color(yg_get_pixel(dst, 6, 6), YG_RGB(128, 0, 127), 1), "opacitate 50%%: %08x", yg_get_pixel(dst, 6, 6));
    /* bucata din sursa, partial in afara destinatiei */
    yg_clear(dst, YG_RGB(0, 0, 0));
    YgRect part = yg_rect(2, 2, 6, 6);
    yg_blit(dst, 36, -2, src, &part);
    CHECK(count_color(dst, YG_RGB(255, 0, 0)) == 4 * 4, "blit decupat: %ld", count_color(dst, YG_RGB(255, 0, 0)));
    /* clip pe destinatie */
    yg_clear(dst, YG_RGB(0, 0, 0));
    yg_set_clip(dst, yg_rect(0, 0, 8, 8));
    yg_blit(dst, 5, 5, src, NULL);
    CHECK(count_color(dst, YG_RGB(255, 0, 0)) == 8 && yg_get_pixel(dst, 5, 5) == YG_ARGB(0, 0, 255, 0), "blit in clip");
    yg_surface_free(dst);
    yg_surface_free(src);
}

static void test_text(void){
    YgFont *f = yg_font_load(YG_FONT_REGULAR);
    CHECK(f != NULL, "font %s", YG_FONT_REGULAR);
    CHECK(yg_font_load("/nu/exista.ttf") == NULL, "font inexistent = NULL");
    if(!f) return;
    YgFont *b = yg_font_load(YG_FONT_BOLD);
    CHECK(b != NULL, "font bold");
    int asc, desc, gap;
    yg_font_vmetrics(f, 32.0f, &asc, &desc, &gap);
    CHECK(asc > 20 && asc < 40 && desc < 0 && desc > -15, "vmetrics %d %d", asc, desc);
    int w1 = yg_text_width(f, 32.0f, "Yokai");
    int w2 = yg_text_width(f, 64.0f, "Yokai");
    CHECK(w1 > 60 && w1 < 120 && near(w2, 2 * w1, 3), "latimi %d %d", w1, w2);
    CHECK(yg_text_width(f, 32.0f, "") == 0, "text gol");
    CHECK(yg_text_width(f, 20.0f, "ab\nabcd") == yg_text_width(f, 20.0f, "abcd"), "mai multe linii: cea mai lata");
    CHECK(yg_text_width(f, 20.0f, "\xC8\x99\xC8\x9B") > 0, "diacritice UTF-8");

    YgSurface *s = yg_surface_new(300, 80);
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_font_cache_clear(f);
    YgCacheStats st0 = yg_font_cache_stats(f);
    int adv = yg_draw_text(s, f, 32.0f, 10, 50, "Yokai \xC8\x98\xC8\x9A", YG_RGB(255, 255, 255));
    YgCacheStats st1 = yg_font_cache_stats(f);
    CHECK(adv > 80 && count_color(s, YG_RGB(255, 255, 255)) > 100, "text desenat (latime %d)", adv);
    CHECK(st1.misses - st0.misses == 8 && st1.glyphs == 8, "8 glife noi: %llu miss, %u in cache",
          (unsigned long long)(st1.misses - st0.misses), st1.glyphs);
    /* text gri antialiased: exista pixeli intermediari */
    int mid = 0;
    for(int y = 0; y < 80; y++)
        for(int x = 0; x < 300; x++){
            int r = YG_R(yg_get_pixel(s, x, y));
            mid += r > 0 && r < 255;
        }
    CHECK(mid > 20, "antialias in text (%d pixeli intermediari)", mid);
    /* nimic deasupra liniei de baza - ascent sau sub baza + descent */
    int stray = 0;
    for(int x = 0; x < 300; x++){
        stray += yg_get_pixel(s, x, 50 - asc - 3) != YG_RGB(0, 0, 0);
        stray += yg_get_pixel(s, x, 50 - desc + 2) != YG_RGB(0, 0, 0);
    }
    CHECK(stray == 0, "glifele raman intre ascent si descent (%d)", stray);
    yg_draw_text(s, f, 32.0f, 10, 50, "Yokai", YG_RGB(255, 255, 255));
    YgCacheStats st2 = yg_font_cache_stats(f);
    CHECK(st2.misses == st1.misses && st2.hits - st1.hits == 5, "a doua oara din cache: %llu hit",
          (unsigned long long)(st2.hits - st1.hits));
    yg_draw_text(s, f, 20.0f, 10, 50, "Yokai", YG_RGB(255, 255, 255));
    YgCacheStats st3 = yg_font_cache_stats(f);
    CHECK(st3.misses - st2.misses == 5, "alta marime = alte glife");

    /* clip pe text */
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_set_clip(s, yg_rect(0, 0, 30, 80));
    yg_draw_text(s, f, 40.0f, 5, 60, "WWWWWW", YG_RGB(255, 255, 255));
    int outside = 0;
    for(int y = 0; y < 80; y++)
        for(int x = 30; x < 300; x++)
            outside += yg_get_pixel(s, x, y) != YG_RGB(0, 0, 0);
    CHECK(outside == 0, "text decupat (%d pixeli afara)", outside);
    yg_reset_clip(s);

    /* limita cache-ului: LRU */
    yg_font_cache_limit(f, 8, 1u << 20);
    YgCacheStats sl = yg_font_cache_stats(f);
    CHECK(sl.glyphs <= 8, "limita aplicata: %u", sl.glyphs);
    yg_draw_text(s, f, 24.0f, 0, 40, "abcdefghijklmnopqrstuvwxyz", YG_RGB(255, 255, 255));
    YgCacheStats se = yg_font_cache_stats(f);
    CHECK(se.glyphs == 8 && se.evictions > sl.evictions, "LRU: %u glife, %llu evacuari", se.glyphs,
          (unsigned long long)se.evictions);
    YgCacheStats sb0 = yg_font_cache_stats(f);
    yg_draw_text(s, f, 24.0f, 0, 40, "z", YG_RGB(255, 255, 255));       /* cea mai recenta: hit */
    yg_draw_text(s, f, 24.0f, 0, 40, "a", YG_RGB(255, 255, 255));       /* cea mai veche: iesise */
    YgCacheStats sb1 = yg_font_cache_stats(f);
    CHECK(sb1.hits - sb0.hits == 1 && sb1.misses - sb0.misses == 1, "LRU pastreaza recentele");
    yg_font_cache_limit(f, 512, 64);
    CHECK(yg_font_cache_stats(f).bytes <= 64, "limita in octeti");
    yg_font_cache_clear(f);
    CHECK(yg_font_cache_stats(f).glyphs == 0 && yg_font_cache_stats(f).bytes == 0, "cache golit");

    YgRect box = yg_rect(0, 0, 300, 80);
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_font_cache_limit(f, 512, 1u << 20);
    yg_draw_text_centered(s, b ? b : f, 30.0f, box, "OK", YG_RGB(255, 255, 255));
    int minx = 300, maxx = -1;
    for(int y = 0; y < 80; y++)
        for(int x = 0; x < 300; x++)
            if(yg_get_pixel(s, x, y) != YG_RGB(0, 0, 0)){
                if(x < minx) minx = x;
                if(x > maxx) maxx = x;
            }
    CHECK(maxx > minx && near((minx + maxx) / 2, 150, 4), "text centrat (%d..%d)", minx, maxx);
    yg_surface_free(s);
    yg_font_free(b);
    yg_font_free(f);
}

static void test_themes_logo(void){
    const YgTheme *d = &yg_theme_dark, *l = &yg_theme_light;
    CHECK(d->background == YG_RGB(0, 0, 0) && d->text == YG_RGB(255, 255, 255), "Dark: fundal negru, text alb");
    CHECK(YG_R(d->logo) > 0xC0 && YG_G(d->logo) < 0x40 && YG_B(d->logo) < 0x40 && d->accent == d->logo, "Dark: logo/accent rosu");
    CHECK(l->background == YG_RGB(255, 255, 255) && l->text == YG_RGB(0, 0, 0), "Light: fundal alb, text negru");
    CHECK(YG_G(l->logo) > YG_R(l->logo) && YG_G(l->logo) > YG_B(l->logo) && YG_G(l->logo) < 0x80, "Light: logo verde inchis");
    CHECK(yg_theme_by_name("dark") == d && yg_theme_by_name("light") == l && !yg_theme_by_name("x"), "teme dupa nume");

    YgSurface *s = yg_surface_new(200, 200);
    yg_clear(s, l->background);
    yg_draw_logo(s, 100.0f, 100.0f, 180.0f, l->logo);
    long n = count_color(s, l->logo);
    CHECK(n > 8000 && n < 25000, "logo: %ld pixeli plini", n);
    /* ochii si gura sunt gauri (fundalul), capul e plin langa ele */
    float k = 1.8f;
#define P(u, v) yg_get_pixel(s, (int)(100 + ((u) - 50) * k), (int)(100 + ((v) - 50) * k))
    CHECK(P(35, 55) == l->background && P(65, 55) == l->background, "ochii decupati");
    CHECK(P(50, 80) == l->background && P(50, 40) == l->background, "gura si rombul decupate");
    CHECK(P(50, 65) == l->logo && P(25, 70) == l->logo && P(14, 8) == l->logo && P(86, 8) == l->logo, "cap si coarne");
    CHECK(P(41, 74) == l->logo, "colt");
    CHECK(yg_get_pixel(s, 2, 198) == l->background && yg_get_pixel(s, 100, 3) == l->background, "in afara logo-ului");
#undef P
    yg_surface_free(s);
}

static void test_fb(void){
    int fd = open("/dev/fb0", O_RDWR);
    CHECK(fd >= 0, "open /dev/fb0: %d", errno);
    if(fd < 0) return;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    CHECK(ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && var.bits_per_pixel == 32 && var.xres >= 320 && var.yres >= 200,
          "FBIOGET_VSCREENINFO %ux%u", var.xres, var.yres);
    CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 && fix.line_length >= var.xres * 4 &&
          fix.smem_len >= fix.line_length * var.yres && fix.smem_len % 4096 == 0 && !strcmp(fix.id, "yokaifb"),
          "FBIOGET_FSCREENINFO pitch %u len %u", fix.line_length, fix.smem_len);
    CHECK(var.red.length == 8 && var.green.length == 8 && var.blue.length == 8, "canale de 8 biti");
    CHECK(ioctl(fd, 0x1234, 0) < 0 && errno == EINVAL, "ioctl necunoscut = EINVAL");
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EINVAL, "MAP_PRIVATE refuzat");
    CHECK(mmap(NULL, fix.smem_len + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENXIO,
          "mmap peste memoria video refuzat");

    YgFb fb;
    int err = yg_fb_open(&fb, NULL);
    CHECK(err == 0 && fb.width == (int)var.xres && fb.pitch == (int)fix.line_length, "yg_fb_open: %d", err);
    if(err){
        close(fd);
        return;
    }
    YgSurface *s = yg_surface_new(fb.width, fb.height);
    yg_clear(s, YG_RGB(0, 0, 0));
    yg_fill_rect(s, 0, 0, 64, 32, YG_RGB(0x11, 0x22, 0x33));
    yg_fill_rect(s, fb.width - 1, fb.height - 1, 1, 1, YG_RGB(0xAB, 0xCD, 0xEF));
    yg_fb_present(&fb, s, NULL);
    /* read() pe /dev/fb0 (pixeli fara padding) vede ce s-a scris prin mapare */
    uint32_t px[2];
    CHECK(lseek(fd, ((off_t)5 * fb.width + 7) * 4, SEEK_SET) >= 0 && read(fd, px, 4) == 4 &&
          (px[0] & 0xFFFFFF) == 0x112233, "pixel citit cu read: %08x", px[0]);
    CHECK(lseek(fd, ((off_t)fb.height * fb.width - 1) * 4, SEEK_SET) >= 0 && read(fd, px, 4) == 4 &&
          (px[0] & 0xFFFFFF) == 0xABCDEF, "ultimul pixel: %08x", px[0]);
    /* write() pe /dev/fb0 se vede prin mapare */
    uint32_t v = 0x00445566;
    CHECK(lseek(fd, (off_t)100 * 4, SEEK_SET) >= 0 && write(fd, &v, 4) == 4 &&
          (((uint32_t *)fb.map)[100] & 0xFFFFFF) == 0x445566, "write se vede in mapare");

    /* fork: copilul scrie prin aceeasi mapare (pagina de dispozitiv, nu CoW) */
    pid_t pid = fork();
    if(pid == 0){
        ((uint32_t *)fb.map)[1] = 0x00778899;
        _exit(0);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork + wait");
    CHECK((((uint32_t *)fb.map)[1] & 0xFFFFFF) == 0x778899, "scrierea copilului se vede la parinte");

    /* desen direct in memoria video */
    YgSurface direct;
    if(yg_fb_surface(&fb, &direct) == 0){
        yg_fill_rect(&direct, 10, 10, 4, 4, YG_RGB(1, 2, 3));
        CHECK((((uint32_t *)(fb.map + 11 * fb.pitch))[11] & 0xFFFFFF) == 0x010203, "suprafata directa pe ecran");
    }

    /* mprotect pe mapare si munmap partial */
    CHECK(mprotect(fb.map, 4096, PROT_READ) == 0 && mprotect(fb.map, 4096, PROT_READ | PROT_WRITE) == 0, "mprotect");
    CHECK(munmap(fb.map + fb.map_len - 4096, 4096) == 0, "munmap partial");
    fb.map_len -= 4096;
    yg_fb_close(&fb);
    yg_surface_free(s);
    close(fd);
}

int main(void){
    test_rects();
    test_surface_clip();
    test_blend();
    test_primitives();
    test_polygons();
    test_blit();
    test_text();
    test_themes_logo();
    test_fb();
    printf("ygfxtest: %d verificari, %d esuate\n", checks, fails);
    if(fails){
        return 1;
    }
    printf("[OK] ygfxtest\n");
    return 0;
}
