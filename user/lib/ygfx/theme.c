/* ygfx: temele YokaiOS ca structuri de culori (ARGB32) */
#include "ygfx.h"
#include <string.h>

/* Dark: fundal negru, text alb, accent si logo rosu */
const YgTheme yg_theme_dark = {
    .name = "dark",
    .background  = YG_RGB(0x00, 0x00, 0x00),
    .surface     = YG_RGB(0x16, 0x16, 0x18),
    .surface_alt = YG_RGB(0x24, 0x24, 0x27),
    .border      = YG_RGB(0x3A, 0x3A, 0x3F),
    .text        = YG_RGB(0xFF, 0xFF, 0xFF),
    .text_muted  = YG_RGB(0xA6, 0xA6, 0xAD),
    .accent      = YG_RGB(0xE0, 0x1B, 0x24),
    .accent_text = YG_RGB(0xFF, 0xFF, 0xFF),
    .logo        = YG_RGB(0xE0, 0x1B, 0x24),
    .shadow      = YG_ARGB(0xA0, 0x00, 0x00, 0x00),
};

/* Light: fundal alb, text negru, logo (si accent) verde inchis */
const YgTheme yg_theme_light = {
    .name = "light",
    .background  = YG_RGB(0xFF, 0xFF, 0xFF),
    .surface     = YG_RGB(0xF2, 0xF3, 0xF2),
    .surface_alt = YG_RGB(0xE4, 0xE7, 0xE4),
    .border      = YG_RGB(0xC8, 0xCC, 0xC8),
    .text        = YG_RGB(0x00, 0x00, 0x00),
    .text_muted  = YG_RGB(0x55, 0x5B, 0x55),
    .accent      = YG_RGB(0x0B, 0x5D, 0x2A),
    .accent_text = YG_RGB(0xFF, 0xFF, 0xFF),
    .logo        = YG_RGB(0x0B, 0x5D, 0x2A),
    .shadow      = YG_ARGB(0x40, 0x00, 0x00, 0x00),
};

const YgTheme *yg_theme_by_name(const char *name){
    if(!name){
        return NULL;
    }
    if(!strcmp(name, "dark")){
        return &yg_theme_dark;
    }
    if(!strcmp(name, "light")){
        return &yg_theme_light;
    }
    return NULL;
}
