#ifndef GFXDEMO_SCENE_H
#define GFXDEMO_SCENE_H
#include "ygfx.h"
/* Scena demo (bara de sus, logo, titlu, butoane, panouri cu primitive / alfa / clipping) in zona area, scalata dupa
 * 1024 x 768, cu tema t */
void scene_draw(YgSurface *s, const YgTheme *t, YgFont *reg, YgFont *bold, YgRect area);
#endif
