/* ygfx: logo-ul Yokai, vectorial: masca de oni (cap rotund, doua coarne curbate), cu ochi oblici, un romb pe frunte
 * si gura decupate (gauri in contur, raman culoarea de dedesubt) si doi colti. Forma e definita intr-o cutie
 * 100 x 100 si se scaleaza; totul trece prin rasterizatorul cu antialias (yg_fill_path). */
#include "ygfx.h"
#include "ygfx_priv.h"
#include <math.h>

#define MAXP 512

typedef struct {
    YgPoint pts[MAXP];
    int counts[16];
    int npts, ncont;
    float cx, cy, k;        /* centrul si scara: 1 unitate = k pixeli */
} Path;

static void begin(Path *p){
    p->counts[p->ncont] = 0;
}

static void pt(Path *p, float u, float v){
    if(p->npts >= MAXP){
        return;
    }
    p->pts[p->npts].x = p->cx + (u - 50.0f) * p->k;
    p->pts[p->npts].y = p->cy + (v - 50.0f) * p->k;
    p->npts++;
    p->counts[p->ncont]++;
}

/* Bezier patratic de la punctul curent (exclus) prin (cu, cv) la (u, v) */
static void quad(Path *p, float u0, float v0, float cu, float cv, float u, float v, int n){
    for(int i = 1; i <= n; i++){
        float t = (float)i / (float)n, it = 1.0f - t;
        pt(p, it * it * u0 + 2 * it * t * cu + t * t * u, it * it * v0 + 2 * it * t * cv + t * t * v);
    }
}

/* Inchide conturul curent; dir = +1 sensul acelor de ceasornic (pe ecran), -1 invers (gaura) */
static void end(Path *p, int dir){
    int n = p->counts[p->ncont];
    YgPoint *c = p->pts + p->npts - n;
    float area = 0.0f;
    for(int i = 0; i < n; i++){
        YgPoint a = c[i], b = c[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    if((area > 0.0f) != (dir > 0)){
        for(int i = 0; i < n / 2; i++){
            YgPoint t = c[i];
            c[i] = c[n - 1 - i];
            c[n - 1 - i] = t;
        }
    }
    p->ncont++;
}

static void circle(Path *p, float u, float v, float r, int dir){
    begin(p);
    for(int i = 0; i < 72; i++){
        double a = 2.0 * YG_PI * i / 72.0;
        pt(p, u + r * (float)cos(a), v + r * (float)sin(a));
    }
    end(p, dir);
}

/* Cornul stang (mirror = 1: cel drept, oglindit fata de u = 50) */
static void horn(Path *p, int mirror){
    float m = mirror ? -1.0f : 1.0f;
#define U(x) (50.0f + m * ((x) - 50.0f))
    begin(p);
    pt(p, U(25.0f), 45.0f);                                             /* baza exterioara */
    quad(p, U(25.0f), 45.0f, U(11.0f), 28.0f, U(13.0f), 3.0f, 14);      /* muchia exterioara pana la varf */
    quad(p, U(13.0f), 3.0f, U(24.0f), 22.0f, U(43.0f), 29.0f, 14);      /* muchia interioara pana la baza */
    end(p, 1);
#undef U
}

static void eye(Path *p, int mirror){
    float m = mirror ? -1.0f : 1.0f;
#define U(x) (50.0f + m * ((x) - 50.0f))
    begin(p);
    pt(p, U(27.0f), 50.0f);
    quad(p, U(27.0f), 50.0f, U(38.0f), 51.0f, U(46.0f), 58.5f, 8);      /* sprinceana oblica spre nas */
    quad(p, U(46.0f), 58.5f, U(40.0f), 64.5f, U(31.0f), 60.0f, 8);
    quad(p, U(31.0f), 60.0f, U(27.0f), 56.0f, U(27.0f), 50.0f, 6);
    end(p, -1);
#undef U
}

void yg_draw_logo(YgSurface *s, float cx, float cy, float size, YgColor c){
    static Path p;
    p.npts = p.ncont = 0;
    p.cx = cx;
    p.cy = cy;
    p.k = size / 100.0f;

    circle(&p, 50.0f, 58.0f, 33.0f, 1);         /* capul */
    horn(&p, 0);
    horn(&p, 1);
    /* romb pe frunte */
    begin(&p);
    pt(&p, 50.0f, 33.0f); pt(&p, 54.5f, 40.0f); pt(&p, 50.0f, 47.0f); pt(&p, 45.5f, 40.0f);
    end(&p, -1);
    eye(&p, 0);
    eye(&p, 1);
    /* gura */
    begin(&p);
    pt(&p, 32.0f, 71.0f);
    pt(&p, 68.0f, 71.0f);
    quad(&p, 68.0f, 71.0f, 64.0f, 86.0f, 50.0f, 86.0f, 10);
    quad(&p, 50.0f, 86.0f, 36.0f, 86.0f, 32.0f, 71.0f, 10);
    end(&p, -1);
    yg_fill_path(s, p.pts, p.counts, p.ncont, c);

    /* coltii, in gura */
    p.npts = p.ncont = 0;
    begin(&p);
    pt(&p, 37.5f, 70.0f); pt(&p, 44.5f, 70.0f); pt(&p, 41.0f, 79.5f);
    end(&p, 1);
    begin(&p);
    pt(&p, 55.5f, 70.0f); pt(&p, 62.5f, 70.0f); pt(&p, 59.0f, 79.5f);
    end(&p, 1);
    yg_fill_path(s, p.pts, p.counts, p.ncont, c);
}
