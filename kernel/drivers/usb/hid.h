#pragma once
#include "types.h"
#include "hal/input.h"

// HID boot protocol (clasa 3, subclasa 1): tastatura (protocol 1) si mouse (protocol 2) pe un endpoint
// interrupt IN. Rapoartele ajung pe event bus (hal/input.h) ca KeyEvent / MouseEvent.
// Cu cmdline "debug=usb" fiecare eveniment se tipareste pe serial ("debug=mouse" doar mouse-ul)

// Inregistreaza driverul USB (din usb_init)
void hid_init();

const int HID_KBD_REPORT = 8;               // modificatori, rezervat, 6 coduri de taste
const int HID_KBD_MAX_EVENTS = 8 + 2 * 6;   // 8 modificatori + 6 eliberari + 6 apasari

// Decodorul raportului de tastatura, separat de hardware ca sa poata fi testat.
// Compara raportul cu cel anterior: tastele disparute = eliberari, cele noi = apasari
struct HidKbdState {
    uint8_t prev[HID_KBD_REPORT];
    bool caps;                  // Caps Lock aprins
};

// Intoarce cate evenimente a scris in out (cel mult HID_KBD_MAX_EVENTS). Raportul "phantom" (rollover, cod 01)
// nu schimba tastele. *leds_changed = Caps Lock s-a comutat (LED-ul trebuie actualizat)
int hid_kbd_decode(HidKbdState& st, const uint8_t* report, KeyEvent* out, bool* leds_changed);
// Raport de mouse: butoane, dx, dy [, roata]; false daca e prea scurt
bool hid_mouse_decode(const uint8_t* report, uint16_t len, MouseEvent& out);
// Codul HID (usage page 7) -> KeyCode, KEY_NONE daca nu e in tabel
uint16_t hid_usage_to_key(uint8_t usage);
