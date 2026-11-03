#ifndef YGFX_PRIV_H
#define YGFX_PRIV_H
/* Interne ygfx (nu se instaleaza in sysroot) */
#include "ygfx.h"

/* c amestecat peste *p cu acoperirea cov (0..255) */
void yg__blend_cov(uint32_t *p, YgColor c, uint32_t cov);

static inline float yg__floor(float v){
    int i = (int)v;
    return (float)(i - (v < (float)i));
}

static inline float yg__ceil(float v){
    int i = (int)v;
    return (float)(i + (v > (float)i));
}

#define YG_PI 3.14159265358979323846

#endif
