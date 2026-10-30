#include "drivers/ps2kbd.h"
#include "arch/x86_64/pic.h"
#include "hal/irqchip.h"
#include "hal/input.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "interrupt.h"
#include "drivers/i8042.h"
#include "io.h"

using namespace i8042;

namespace {

const uint8_t KBD_SET_LEDS = 0xED;
const uint8_t KBD_SCANCODE_SET = 0xF0;
const uint8_t KBD_ENABLE_SCAN = 0xF4;
const uint8_t KBD_ACK = 0xFA;
const uint8_t SCANCODE_SET_1 = 0x01;

const uint8_t LED_CAPS = 0x04;
const uint8_t BREAK_BIT = 0x80;
const uint8_t PREFIX_E0 = 0xE0;
const uint8_t PREFIX_E1 = 0xE1;
const uint8_t PAUSE_EXTRA_BYTES = 5;    // Pause = E1 1D 45 E1 9D C5, ignoram cei 5 octeti de dupa E1

// Stare de Shift/Ctrl/Alt pe fiecare parte, ca sa stim cand se elibereaza ultima
enum : uint8_t {
    HELD_LSHIFT = 1 << 0,
    HELD_RSHIFT = 1 << 1,
    HELD_LCTRL  = 1 << 2,
    HELD_RCTRL  = 1 << 3,
    HELD_LALT   = 1 << 4,
    HELD_RALT   = 1 << 5,
};

struct KeyDef {
    uint8_t scancode;   // set 1, fara bitul de break
    KeyCode code;
    char lower;
    char upper;
    bool letter;        // litera: Caps Lock inverseaza Shift
};

// Scancode set 1, fara prefix
const KeyDef base_keys[] = {
    {0x01, KEY_ESC, 0, 0, false},
    {0x02, KEY_1, '1', '!', false},
    {0x03, KEY_2, '2', '@', false},
    {0x04, KEY_3, '3', '#', false},
    {0x05, KEY_4, '4', '$', false},
    {0x06, KEY_5, '5', '%', false},
    {0x07, KEY_6, '6', '^', false},
    {0x08, KEY_7, '7', '&', false},
    {0x09, KEY_8, '8', '*', false},
    {0x0A, KEY_9, '9', '(', false},
    {0x0B, KEY_0, '0', ')', false},
    {0x0C, KEY_MINUS, '-', '_', false},
    {0x0D, KEY_EQUAL, '=', '+', false},
    {0x0E, KEY_BACKSPACE, '\b', '\b', false},
    {0x0F, KEY_TAB, '\t', '\t', false},
    {0x10, KEY_Q, 'q', 'Q', true},
    {0x11, KEY_W, 'w', 'W', true},
    {0x12, KEY_E, 'e', 'E', true},
    {0x13, KEY_R, 'r', 'R', true},
    {0x14, KEY_T, 't', 'T', true},
    {0x15, KEY_Y, 'y', 'Y', true},
    {0x16, KEY_U, 'u', 'U', true},
    {0x17, KEY_I, 'i', 'I', true},
    {0x18, KEY_O, 'o', 'O', true},
    {0x19, KEY_P, 'p', 'P', true},
    {0x1A, KEY_LBRACKET, '[', '{', false},
    {0x1B, KEY_RBRACKET, ']', '}', false},
    {0x1C, KEY_ENTER, '\n', '\n', false},
    {0x1D, KEY_LCTRL, 0, 0, false},
    {0x1E, KEY_A, 'a', 'A', true},
    {0x1F, KEY_S, 's', 'S', true},
    {0x20, KEY_D, 'd', 'D', true},
    {0x21, KEY_F, 'f', 'F', true},
    {0x22, KEY_G, 'g', 'G', true},
    {0x23, KEY_H, 'h', 'H', true},
    {0x24, KEY_J, 'j', 'J', true},
    {0x25, KEY_K, 'k', 'K', true},
    {0x26, KEY_L, 'l', 'L', true},
    {0x27, KEY_SEMICOLON, ';', ':', false},
    {0x28, KEY_APOSTROPHE, '\'', '"', false},
    {0x29, KEY_GRAVE, '`', '~', false},
    {0x2A, KEY_LSHIFT, 0, 0, false},
    {0x2B, KEY_BACKSLASH, '\\', '|', false},
    {0x2C, KEY_Z, 'z', 'Z', true},
    {0x2D, KEY_X, 'x', 'X', true},
    {0x2E, KEY_C, 'c', 'C', true},
    {0x2F, KEY_V, 'v', 'V', true},
    {0x30, KEY_B, 'b', 'B', true},
    {0x31, KEY_N, 'n', 'N', true},
    {0x32, KEY_M, 'm', 'M', true},
    {0x33, KEY_COMMA, ',', '<', false},
    {0x34, KEY_DOT, '.', '>', false},
    {0x35, KEY_SLASH, '/', '?', false},
    {0x36, KEY_RSHIFT, 0, 0, false},
    {0x38, KEY_LALT, 0, 0, false},
    {0x39, KEY_SPACE, ' ', ' ', false},
    {0x3A, KEY_CAPSLOCK, 0, 0, false},
    {0x3B, KEY_F1, 0, 0, false},
    {0x3C, KEY_F2, 0, 0, false},
    {0x3D, KEY_F3, 0, 0, false},
    {0x3E, KEY_F4, 0, 0, false},
    {0x3F, KEY_F5, 0, 0, false},
    {0x40, KEY_F6, 0, 0, false},
    {0x41, KEY_F7, 0, 0, false},
    {0x42, KEY_F8, 0, 0, false},
    {0x43, KEY_F9, 0, 0, false},
    {0x44, KEY_F10, 0, 0, false},
    {0x57, KEY_F11, 0, 0, false},
    {0x58, KEY_F12, 0, 0, false},
};

// Scancode set 1 dupa prefixul E0 (tastele de navigare, Win, Ctrl/Alt dreapta)
const KeyDef extended_keys[] = {
    {0x1C, KEY_ENTER, '\n', '\n', false},   // Enter de pe tastatura numerica
    {0x1D, KEY_RCTRL, 0, 0, false},
    {0x35, KEY_SLASH, '/', '?', false},     // / de pe tastatura numerica
    {0x38, KEY_RALT, 0, 0, false},
    {0x47, KEY_HOME, 0, 0, false},
    {0x48, KEY_UP, 0, 0, false},
    {0x49, KEY_PGUP, 0, 0, false},
    {0x4B, KEY_LEFT, 0, 0, false},
    {0x4D, KEY_RIGHT, 0, 0, false},
    {0x4F, KEY_END, 0, 0, false},
    {0x50, KEY_DOWN, 0, 0, false},
    {0x51, KEY_PGDN, 0, 0, false},
    {0x52, KEY_INSERT, 0, 0, false},
    {0x53, KEY_DELETE, 0, 0, false},
    {0x5B, KEY_LWIN, 0, 0, false},
    {0x5C, KEY_RWIN, 0, 0, false},
    {0x5D, KEY_MENU, 0, 0, false},
};

class Ps2Keyboard final : public InputDevice {
public:
    constexpr Ps2Keyboard() = default;
    InputKind kind() const override { return InputKind::Keyboard; }
};

Ps2Keyboard keyboard;

uint8_t held = 0;               // HELD_* pentru modificatorii tinuti apasati
bool caps = false;              // starea Caps Lock (comutata la fiecare apasare)
bool caps_down = false;         // Caps Lock e tinut apasat; evita comutarea la autorepeat
bool extended = false;          // a venit prefixul E0 pentru octetul urmator
uint8_t pause_skip = 0;         // octeti din secventa Pause de ignorat

template <uint32_t N>
const KeyDef* find_key(const KeyDef (&table)[N], uint8_t scancode){
    for(uint32_t i = 0; i < N; i++){
        if(table[i].scancode == scancode){
            return &table[i];
        }
    }
    return nullptr;
}

uint8_t held_bit(KeyCode code){
    switch(code){
        case KEY_LSHIFT: return HELD_LSHIFT;
        case KEY_RSHIFT: return HELD_RSHIFT;
        case KEY_LCTRL:  return HELD_LCTRL;
        case KEY_RCTRL:  return HELD_RCTRL;
        case KEY_LALT:   return HELD_LALT;
        case KEY_RALT:   return HELD_RALT;
        default:         return 0;
    }
}

uint8_t modifiers(){
    uint8_t m = 0;
    if(held & (HELD_LSHIFT | HELD_RSHIFT)){
        m |= MOD_SHIFT;
    }
    if(held & (HELD_LCTRL | HELD_RCTRL)){
        m |= MOD_CTRL;
    }
    if(held & (HELD_LALT | HELD_RALT)){
        m |= MOD_ALT;
    }
    if(caps){
        m |= MOD_CAPS;
    }
    return m;
}

// Trimite un octet tastaturii si asteapta ACK (0xFA); folosit doar la init, cand IRQ1 inca nu e activ
bool kbd_cmd_sync(uint8_t byte){
    data_write(byte);
    for(uint32_t i = 0; i < WAIT_LOOPS; i++){
        if(inb(KBD_STATUS) & STATUS_OUTPUT_FULL){
            if(inb(KBD_DATA) == KBD_ACK){
                return true;
            }
        }
    }
    return false;
}

// Aprinde/stinge LED-urile tastaturii. Fara ACK: ACK-ul vine pe IRQ1 si e ignorat (nu e in tabel)
void kbd_set_leds(uint8_t mask){
    data_write(KBD_SET_LEDS);
    data_write(mask);
}

void kbd_irq(InterruptFrame*){
    uint8_t byte = inb(KBD_DATA);
    if(pause_skip > 0){
        pause_skip--;
        return;
    }
    if(byte == PREFIX_E0){
        extended = true;
        return;
    }
    if(byte == PREFIX_E1){
        pause_skip = PAUSE_EXTRA_BYTES;
        return;
    }

    bool pressed = !(byte & BREAK_BIT);
    uint8_t scancode = byte & ~BREAK_BIT;
    const KeyDef* def = extended ? find_key(extended_keys, scancode) : find_key(base_keys, scancode);
    extended = false;
    if(!def){
        return;
    }

    KeyCode code = def->code;
    uint8_t bit = held_bit(code);
    if(pressed){
        held |= bit;
    } else {
        held &= ~bit;
    }

    if(code == KEY_CAPSLOCK){
        if(pressed && !caps_down){
            caps = !caps;
            kbd_set_leds(caps ? LED_CAPS : 0);
        }
        caps_down = pressed;
    }

    uint8_t mods = modifiers();
    char ascii = 0;
    if(pressed){
        bool up = (mods & MOD_SHIFT) != 0;
        if(def->letter && (mods & MOD_CAPS)){
            up = !up;
        }
        ascii = up ? def->upper : def->lower;
    }

    InputEvent ev;
    ev.type = InputType::Key;
    ev.raw = byte;
    ev.code = 0;
    ev.value = 0;
    ev.time_ns = monotonic_ns();
    ev.key.keycode = code;
    ev.key.ascii = ascii;
    ev.key.pressed = pressed;
    ev.key.modifiers = mods;
    input_push(ev);
}

}

void ps2kbd_init(){
    if(!present()){
        kprintf("ps2kbd: fara controler 8042\n");
        return;
    }
    // Controlerul 8042: opreste porturile, goleste bufferul, self-test
    ctrl_cmd(CTRL_DISABLE_P1);
    ctrl_cmd(CTRL_DISABLE_P2);
    flush_output();

    ctrl_cmd(CTRL_SELF_TEST);
    if(!wait_output() || inb(KBD_DATA) != SELF_TEST_OK){
        kprintf("[FAIL] ps2kbd: self-test controler\n");
        return;
    }

    // Self-test-ul poate reseta configuratia: o citim, o modificam si o scriem inapoi
    ctrl_cmd(CTRL_READ_CONFIG);
    if(!wait_output()){
        kprintf("[FAIL] ps2kbd: nu citesc configuratia\n");
        return;
    }
    uint8_t config = inb(KBD_DATA);
    config &= ~(CFG_IRQ1 | CFG_TRANSLATE | CFG_P1_CLOCK_OFF);
    ctrl_cmd(CTRL_WRITE_CONFIG);
    data_write(config);

    ctrl_cmd(CTRL_P1_TEST);
    if(!wait_output() || inb(KBD_DATA) != 0){
        kprintf("[FAIL] ps2kbd: testul portului 1\n");
        return;
    }
    ctrl_cmd(CTRL_ENABLE_P1);

    // Tastatura: scancode set 1, LED-urile stinse, apoi pornim scanarea
    if(!kbd_cmd_sync(KBD_SCANCODE_SET) || !kbd_cmd_sync(SCANCODE_SET_1)){
        kprintf("[WARN] ps2kbd: setul de scancode nu a fost confirmat\n");
    }
    kbd_cmd_sync(KBD_SET_LEDS);
    kbd_cmd_sync(0);
    if(!kbd_cmd_sync(KBD_ENABLE_SCAN)){
        kprintf("[WARN] ps2kbd: scanarea nu a fost confirmata\n");
    }

    // Abia acum activam IRQ1 pe controler si pe linia de intrerupere
    flush_output();
    ctrl_cmd(CTRL_WRITE_CONFIG);
    data_write(config | CFG_IRQ1);

    hal_register(HalKind::Input, &keyboard, "ps2-kbd");
    irq_register(PIC_IRQ_BASE + KBD_IRQ, kbd_irq);
    InterruptController* chip = hal_irqchip();
    chip->route_irq(KBD_IRQ, PIC_IRQ_BASE + KBD_IRQ, chip->cpu_id());
}
