/* gfxdemo (T10.1): deseneaza scena ygfx direct in /dev/fb0, cu ambele teme.
 *   gfxdemo [-t dark|light|split|cycle] [-s secunde] [-k] [-d]
 *   -t  ce se arata (implicit cycle: Dark, Light, apoi ambele una langa alta)
 *   -s  cat ramane fiecare ecran (implicit 4 s; 0 = fara pauza)
 *   -k  asteapta Enter intre ecrane in loc de pauza
 *   -d  deseneaza direct in memoria video (fara back buffer in RAM; doar pentru formatul xRGB)
 * La iesire maparea dispare si consola isi reface ecranul. */
#include "ygfx.h"
#include "scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void usage(void){
    fprintf(stderr, "folosire: gfxdemo [-t dark|light|split|cycle] [-s secunde] [-k] [-d]\n");
    exit(2);
}

static void wait_step(int secs, int key){
    if(key){
        char buf[16];
        if(read(0, buf, sizeof(buf)) < 0){
            sleep(1);
        }
    } else if(secs > 0){
        sleep((unsigned)secs);
    }
}

int main(int argc, char **argv){
    const char *mode = "cycle";
    int secs = 4, key = 0, direct = 0;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "-t") && i + 1 < argc){
            mode = argv[++i];
        } else if(!strcmp(argv[i], "-s") && i + 1 < argc){
            secs = atoi(argv[++i]);
        } else if(!strcmp(argv[i], "-k")){
            key = 1;
        } else if(!strcmp(argv[i], "-d")){
            direct = 1;
        } else {
            usage();
        }
    }
    int cycle = !strcmp(mode, "cycle");
    if(!cycle && strcmp(mode, "split") && !yg_theme_by_name(mode)){
        usage();
    }

    YgFont *reg = yg_font_load(YG_FONT_REGULAR);
    YgFont *bold = yg_font_load(YG_FONT_BOLD);
    if(!reg || !bold){
        fprintf(stderr, "gfxdemo: nu pot incarca fonturile din %s\n", YG_FONT_DIR);
        return 1;
    }
    YgFb fb;
    int err = yg_fb_open(&fb, NULL);
    if(err){
        fprintf(stderr, "gfxdemo: /dev/fb0: %s\n", strerror(-err));
        return 1;
    }
    printf("gfxdemo: ecran %dx%d, pitch %d, format %s\n", fb.width, fb.height, fb.pitch, fb.native ? "xRGB8888" : "convertit");

    YgSurface direct_s, *back = NULL;
    if(direct && yg_fb_surface(&fb, &direct_s) == 0){
        back = &direct_s;
    } else {
        direct = 0;
        back = yg_surface_new(fb.width, fb.height);
        if(!back){
            fprintf(stderr, "gfxdemo: fara memorie pentru back buffer\n");
            yg_fb_close(&fb);
            return 1;
        }
    }

    const char *steps[3];
    int nsteps = 0;
    if(cycle){
        steps[nsteps++] = "dark";
        steps[nsteps++] = "light";
        steps[nsteps++] = "split";
    } else {
        steps[nsteps++] = mode;
    }
    YgRect all = yg_rect(0, 0, fb.width, fb.height);
    for(int i = 0; i < nsteps; i++){
        long t0 = now_ms();
        if(!strcmp(steps[i], "split")){
            int half = fb.width / 2;
            scene_draw(back, &yg_theme_dark, reg, bold, yg_rect(0, 0, half, fb.height));
            scene_draw(back, &yg_theme_light, reg, bold, yg_rect(half, 0, fb.width - half, fb.height));
        } else {
            scene_draw(back, yg_theme_by_name(steps[i]), reg, bold, all);
        }
        long t1 = now_ms();
        if(!direct){
            yg_fb_present(&fb, back, NULL);
        }
        long t2 = now_ms();
        printf("gfxdemo: %s desenat in %ld ms, copiat pe ecran in %ld ms\n", steps[i], t1 - t0, t2 - t1);
        fflush(stdout);
        wait_step(secs, key);
    }
    YgCacheStats st = yg_font_cache_stats(reg);
    printf("gfxdemo: cache glife (regular): %u glife, %u octeti, %llu hit, %llu miss, %llu evacuari\n", st.glyphs, st.bytes,
           (unsigned long long)st.hits, (unsigned long long)st.misses, (unsigned long long)st.evictions);

    if(!direct){
        yg_surface_free(back);
    }
    yg_fb_close(&fb);
    yg_font_free(reg);
    yg_font_free(bold);
    printf("[OK] gfxdemo\n");
    return 0;
}
