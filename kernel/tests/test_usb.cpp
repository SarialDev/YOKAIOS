#include "tests/test_usb.h"
#include "drivers/usb/usb.h"
#include "drivers/usb/hid.h"
#include "hal/hal.h"
#include "lib/kprintf.h"
#include "core/panic.h"

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_usb: %s (linia %d)", #cond, __LINE__); } while(0)

const uint8_t MOD_LSHIFT_BIT = 1 << 1;

int decode(HidKbdState& st, const uint8_t (&r)[HID_KBD_REPORT], KeyEvent* ev, bool* leds = nullptr){
    return hid_kbd_decode(st, r, ev, leds);
}

void check_keyboard(){
    HidKbdState st = {};
    KeyEvent ev[HID_KBD_MAX_EVENTS];
    bool leds;

    CHECK(hid_usage_to_key(0x04) == KEY_A && hid_usage_to_key(0x1D) == KEY_Z && hid_usage_to_key(0x28) == KEY_ENTER);
    CHECK(hid_usage_to_key(0x52) == KEY_UP && hid_usage_to_key(0xE0) == KEY_NONE);

    // "a" apasat, apoi si "b", apoi "a" eliberat, apoi nimic
    const uint8_t r1[HID_KBD_REPORT] = {0, 0, 0x04};
    CHECK(decode(st, r1, ev) == 1);
    CHECK(ev[0].keycode == KEY_A && ev[0].ascii == 'a' && ev[0].pressed && ev[0].modifiers == 0);
    const uint8_t r2[HID_KBD_REPORT] = {0, 0, 0x04, 0x05};
    CHECK(decode(st, r2, ev) == 1 && ev[0].keycode == KEY_B && ev[0].pressed);
    const uint8_t r3[HID_KBD_REPORT] = {0, 0, 0x05};
    CHECK(decode(st, r3, ev) == 1 && ev[0].keycode == KEY_A && !ev[0].pressed && ev[0].ascii == 0);
    const uint8_t r4[HID_KBD_REPORT] = {};
    CHECK(decode(st, r4, ev) == 1 && ev[0].keycode == KEY_B && !ev[0].pressed);
    CHECK(decode(st, r4, ev) == 0);

    // Shift stang + "1" -> '!'; modificatorul vine ca eveniment separat
    const uint8_t r5[HID_KBD_REPORT] = {MOD_LSHIFT_BIT, 0, 0x1E};
    CHECK(decode(st, r5, ev) == 2);
    CHECK(ev[0].keycode == KEY_LSHIFT && ev[0].pressed && (ev[0].modifiers & MOD_SHIFT));
    CHECK(ev[1].keycode == KEY_1 && ev[1].ascii == '!' && (ev[1].modifiers & MOD_SHIFT));
    CHECK(decode(st, r4, ev) == 2 && ev[0].keycode == KEY_LSHIFT && !ev[0].pressed && !ev[1].pressed);

    // Caps Lock: comuta starea, cere LED-ul, literele devin mari, Shift le inverseaza
    const uint8_t caps[HID_KBD_REPORT] = {0, 0, 0x39};
    CHECK(decode(st, caps, ev, &leds) == 1 && leds && st.caps && (ev[0].modifiers & MOD_CAPS));
    CHECK(decode(st, r4, ev, &leds) == 1 && !leds && st.caps);
    CHECK(decode(st, r1, ev) == 1 && ev[0].ascii == 'A');
    CHECK(decode(st, r4, ev) == 1);
    const uint8_t shift_a[HID_KBD_REPORT] = {MOD_LSHIFT_BIT, 0, 0x04};
    CHECK(decode(st, shift_a, ev) == 2 && ev[1].ascii == 'a');
    CHECK(decode(st, r4, ev) == 2);
    CHECK(decode(st, caps, ev, &leds) == 1 && leds && !st.caps);
    CHECK(decode(st, r4, ev) == 1);

    // Rollover (01 peste tot): tastele raman cum erau
    CHECK(decode(st, r1, ev) == 1);
    const uint8_t phantom[HID_KBD_REPORT] = {0, 0, 1, 1, 1, 1, 1, 1};
    CHECK(decode(st, phantom, ev) == 0);
    CHECK(decode(st, r4, ev) == 1 && ev[0].keycode == KEY_A && !ev[0].pressed);

    // Enter si Backspace au caracterele de control, ca la PS/2
    const uint8_t enter[HID_KBD_REPORT] = {0, 0, 0x28, 0x2A};
    CHECK(decode(st, enter, ev) == 2 && ev[0].ascii == '\n' && ev[1].ascii == '\b');
    decode(st, r4, ev);
    kprintf("[OK] test_usb hid kbd\n");
}

void check_mouse(){
    MouseEvent m;
    const uint8_t r1[] = {0x01, 5, 0xFD};
    CHECK(hid_mouse_decode(r1, 3, m) && m.dx == 5 && m.dy == -3 && m.dz == 0 && m.buttons == MOUSE_LEFT);
    const uint8_t r2[] = {0x06, 0x80, 0x7F, 0x01};
    CHECK(hid_mouse_decode(r2, 4, m) && m.dx == -128 && m.dy == 127 && m.dz == -1);
    CHECK(m.buttons == (MOUSE_RIGHT | MOUSE_MIDDLE));
    CHECK(!hid_mouse_decode(r2, 2, m));
    kprintf("[OK] test_usb hid mouse\n");
}

// Cu xHCI (QEMU -device qemu-xhci -device usb-kbd -device usb-mouse) cele doua apar in HAL
void check_devices(){
    if(xhci_controller_count() == 0){
        kprintf("test_usb: fara xHCI, sar peste dispozitive\n");
        return;
    }
    bool kbd = false, mouse = false;
    for(int i = 0; i < hal_count(HalKind::Input); i++){
        const char* n = hal_name(HalKind::Input, i);
        if(n[0] == 'u' && n[4] == 'k'){
            kbd = true;
        }
        if(n[0] == 'u' && n[4] == 'm'){
            mouse = true;
        }
    }
    kprintf("test_usb: %d dispozitive, usb-kbd %s, usb-mouse %s\n", usb_device_count(), kbd ? "da" : "nu",
            mouse ? "da" : "nu");
    CHECK(usb_device_count() >= 2 && kbd && mouse);
    kprintf("[OK] test_usb devices\n");
}

}

void test_usb_run(){
    check_keyboard();
    check_mouse();
    check_devices();
    kprintf("[OK] test_usb (hid kbd, hid mouse)\n");
}
