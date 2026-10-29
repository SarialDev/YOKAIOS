#pragma once
#include "types.h"

// Codurile logice ale tastelor (independente de scancode si de layout)
enum KeyCode : uint16_t {
    KEY_NONE = 0,
    KEY_ESC,
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LBRACKET, KEY_RBRACKET, KEY_ENTER,
    KEY_LCTRL,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
    KEY_LSHIFT, KEY_BACKSLASH,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RSHIFT,
    KEY_LALT, KEY_SPACE, KEY_CAPSLOCK,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_RCTRL, KEY_RALT,
    KEY_HOME, KEY_UP, KEY_PGUP, KEY_LEFT, KEY_RIGHT, KEY_END, KEY_DOWN, KEY_PGDN,
    KEY_INSERT, KEY_DELETE,
    KEY_LWIN, KEY_RWIN, KEY_MENU,
};

// Modificatorii activi in momentul evenimentului (bitmask)
enum KeyMod : uint8_t {
    MOD_SHIFT = 1 << 0,
    MOD_CTRL  = 1 << 1,
    MOD_ALT   = 1 << 2,
    MOD_CAPS  = 1 << 3,     // Caps Lock aprins (LED-ul e aprins si el)
};
