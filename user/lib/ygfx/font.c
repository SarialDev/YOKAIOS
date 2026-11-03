/* ygfx: text TrueType (stb_truetype) cu cache LRU de glife.
 * O glifa din cache = bitmap-ul de acoperire (0..255) al unui codepoint la o marime (px * 64, intreg), plus
 * deplasarea fata de punctul de pe linia de baza si avansul. Tabela hash pe (codepoint, marime) + lista LRU dublu
 * inlantuita; la depasirea limitelor (glife sau bytes) iese cea mai veche folosita. */
#include "ygfx.h"
#include "ygfx_priv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define HASH_SIZE 1024
#define DEFAULT_MAX_GLYPHS 512
#define DEFAULT_MAX_BYTES (1u << 20)

typedef struct Glyph {
    uint32_t cp;
    uint32_t size64;            /* px * 64 */
    int w, h, xoff, yoff;       /* bitmap si coltul lui fata de punctul de baza */
    float advance;              /* in pixeli */
    uint8_t *bitmap;
    struct Glyph *hnext;        /* lantul din tabela hash */
    struct Glyph *prev, *next;  /* LRU: head = cea mai recenta */
} Glyph;

struct YgFont {
    stbtt_fontinfo info;
    uint8_t *owned;             /* datele TTF, daca le-a citit yg_font_load */
    Glyph *table[HASH_SIZE];
    Glyph *lru_head, *lru_tail;
    YgCacheStats st;
};

static uint32_t hash(uint32_t cp, uint32_t size64){
    uint32_t h = cp * 2654435761u ^ size64 * 40503u;
    return (h ^ (h >> 15)) & (HASH_SIZE - 1);
}

static void lru_unlink(YgFont *f, Glyph *g){
    if(g->prev) g->prev->next = g->next; else f->lru_head = g->next;
    if(g->next) g->next->prev = g->prev; else f->lru_tail = g->prev;
    g->prev = g->next = NULL;
}

static void lru_push(YgFont *f, Glyph *g){
    g->prev = NULL;
    g->next = f->lru_head;
    if(f->lru_head) f->lru_head->prev = g;
    f->lru_head = g;
    if(!f->lru_tail) f->lru_tail = g;
}

static void glyph_drop(YgFont *f, Glyph *g){
    Glyph **pp = &f->table[hash(g->cp, g->size64)];
    while(*pp != g){
        pp = &(*pp)->hnext;
    }
    *pp = g->hnext;
    lru_unlink(f, g);
    f->st.glyphs--;
    f->st.bytes -= (uint32_t)(g->w * g->h);
    free(g->bitmap);
    free(g);
}

/* Scoate cele mai vechi glife pana cand incape inca una de extra bytes */
static void evict(YgFont *f, uint32_t extra){
    while(f->lru_tail && (f->st.glyphs + 1 > f->st.max_glyphs || f->st.bytes + extra > f->st.max_bytes)){
        glyph_drop(f, f->lru_tail);
        f->st.evictions++;
    }
}

static float scale_for(YgFont *f, float px){
    return stbtt_ScaleForPixelHeight(&f->info, px);
}

static Glyph *glyph_get(YgFont *f, uint32_t cp, float px){
    uint32_t size64 = (uint32_t)(px * 64.0f + 0.5f);
    uint32_t h = hash(cp, size64);
    for(Glyph *g = f->table[h]; g; g = g->hnext){
        if(g->cp == cp && g->size64 == size64){
            f->st.hits++;
            lru_unlink(f, g);
            lru_push(f, g);
            return g;
        }
    }
    f->st.misses++;
    float scale = scale_for(f, (float)size64 / 64.0f);
    int gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&f->info, gi, scale, scale, &x0, &y0, &x1, &y1);
    Glyph *g = calloc(1, sizeof(*g));
    if(!g){
        return NULL;
    }
    g->cp = cp;
    g->size64 = size64;
    g->w = x1 - x0;
    g->h = y1 - y0;
    g->xoff = x0;
    g->yoff = y0;
    g->advance = (float)adv * scale;
    if(g->w > 0 && g->h > 0){
        g->bitmap = malloc((size_t)g->w * (size_t)g->h);
        if(!g->bitmap){
            free(g);
            return NULL;
        }
        stbtt_MakeGlyphBitmap(&f->info, g->bitmap, g->w, g->h, g->w, scale, scale, gi);
    } else {
        g->w = g->h = 0;
    }
    evict(f, (uint32_t)(g->w * g->h));
    g->hnext = f->table[h];
    f->table[h] = g;
    lru_push(f, g);
    f->st.glyphs++;
    f->st.bytes += (uint32_t)(g->w * g->h);
    return g;
}

static YgFont *font_new(const uint8_t *data, size_t len){
    if(!data || len < 12){
        return NULL;
    }
    YgFont *f = calloc(1, sizeof(*f));
    if(!f){
        return NULL;
    }
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if(off < 0 || !stbtt_InitFont(&f->info, data, off)){
        free(f);
        return NULL;
    }
    f->st.max_glyphs = DEFAULT_MAX_GLYPHS;
    f->st.max_bytes = DEFAULT_MAX_BYTES;
    return f;
}

YgFont *yg_font_from_memory(const uint8_t *data, size_t len){
    return font_new(data, len);
}

YgFont *yg_font_load(const char *path){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return NULL;
    }
    struct stat sb;
    if(fstat(fd, &sb) < 0 || sb.st_size <= 0 || sb.st_size > (64 << 20)){
        close(fd);
        return NULL;
    }
    size_t len = (size_t)sb.st_size;
    uint8_t *data = malloc(len);
    size_t got = 0;
    while(data && got < len){
        ssize_t n = read(fd, data + got, len - got);
        if(n <= 0){
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    if(!data || got != len){
        free(data);
        return NULL;
    }
    YgFont *f = font_new(data, len);
    if(!f){
        free(data);
        return NULL;
    }
    f->owned = data;
    return f;
}

void yg_font_cache_clear(YgFont *f){
    while(f->lru_tail){
        glyph_drop(f, f->lru_tail);
    }
}

void yg_font_cache_limit(YgFont *f, uint32_t max_glyphs, uint32_t max_bytes){
    f->st.max_glyphs = max_glyphs ? max_glyphs : 1;
    f->st.max_bytes = max_bytes;
    while(f->lru_tail && (f->st.glyphs > f->st.max_glyphs || f->st.bytes > f->st.max_bytes)){
        glyph_drop(f, f->lru_tail);
        f->st.evictions++;
    }
}

YgCacheStats yg_font_cache_stats(const YgFont *f){
    return f->st;
}

void yg_font_free(YgFont *f){
    if(!f){
        return;
    }
    yg_font_cache_clear(f);
    free(f->owned);
    free(f);
}

void yg_font_vmetrics(YgFont *f, float px, int *ascent, int *descent, int *line_gap){
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    float sc = scale_for(f, px);
    if(ascent) *ascent = (int)yg__ceil((float)a * sc);
    if(descent) *descent = (int)yg__floor((float)d * sc);
    if(line_gap) *line_gap = (int)((float)g * sc + 0.5f);
}

/* Urmatorul codepoint UTF-8 (U+FFFD pentru secvente invalide) */
static uint32_t utf8_next(const char **ps){
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    int extra = 0;
    if(c >= 0xF0 && c < 0xF8){ c &= 0x07; extra = 3; }
    else if(c >= 0xE0){ c &= 0x0F; extra = 2; }
    else if(c >= 0xC0){ c &= 0x1F; extra = 1; }
    else if(c >= 0x80){ *ps = (const char *)s; return 0xFFFD; }
    for(int i = 0; i < extra; i++){
        if((*s & 0xC0) != 0x80){
            *ps = (const char *)s;
            return 0xFFFD;
        }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *ps = (const char *)s;
    return c;
}

/* Parcurge o linie: deseneaza daca s != NULL; intoarce latimea si pozitia de dupa linie */
static float line_run(YgSurface *s, YgFont *f, float px, float x, int y, const char **pp, YgColor c){
    const char *p = *pp;
    float scale = scale_for(f, px);
    float pen = x;
    uint32_t prev = 0;
    while(*p && *p != '\n'){
        uint32_t cp = utf8_next(&p);
        if(prev){
            pen += (float)stbtt_GetCodepointKernAdvance(&f->info, (int)prev, (int)cp) * scale;
        }
        Glyph *g = glyph_get(f, cp, px);
        if(g){
            if(s && g->bitmap){
                int gx = (int)yg__floor(pen + 0.5f) + g->xoff;
                yg_fill_mask(s, gx, y + g->yoff, g->bitmap, g->w, g->h, g->w, c);
            }
            pen += g->advance;
        }
        prev = cp;
    }
    *pp = p;
    return pen - x;
}

int yg_text_width(YgFont *f, float px, const char *utf8){
    float best = 0.0f;
    const char *p = utf8;
    for(;;){
        float w = line_run(NULL, f, px, 0.0f, 0, &p, 0);
        if(w > best) best = w;
        if(*p != '\n') break;
        p++;
    }
    return (int)yg__ceil(best);
}

int yg_draw_text(YgSurface *s, YgFont *f, float px, int x, int y, const char *utf8, YgColor c){
    int asc, desc, gap;
    yg_font_vmetrics(f, px, &asc, &desc, &gap);
    const char *p = utf8;
    float w = 0.0f;
    for(;;){
        w = line_run(s, f, px, (float)x, y, &p, c);
        if(*p != '\n') break;
        p++;
        y += asc - desc + gap;
    }
    return (int)yg__ceil(w);
}

void yg_draw_text_centered(YgSurface *s, YgFont *f, float px, YgRect r, const char *utf8, YgColor c){
    int asc, desc;
    yg_font_vmetrics(f, px, &asc, &desc, NULL);
    int w = yg_text_width(f, px, utf8);
    int x = r.x + (r.w - w) / 2;
    int y = r.y + (r.h - (asc - desc)) / 2 + asc;
    yg_draw_text(s, f, px, x, y, utf8, c);
}
