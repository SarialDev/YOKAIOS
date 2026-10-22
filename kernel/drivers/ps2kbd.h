#pragma once
#include "types.h"

const uint8_t KBD_IRQ = 1;

// Apelat din IRQ1 cu scancode-ul brut (set 1)
typedef void (*KeyCallback)(uint8_t scancode);

void ps2kbd_init(KeyCallback cb);
