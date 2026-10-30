#include "drivers/usb/hid.h"
#include "drivers/usb/usb.h"
#include "boot/multiboot2.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace {

const uint8_t HID_CLASS = 0x03;
const uint8_t HID_SUBCLASS_BOOT = 0x01;
const uint8_t HID_PROTO_KEYBOARD = 0x01;
const uint8_t HID_PROTO_MOUSE = 0x02;

const uint8_t HID_REQ_SET_REPORT = 0x09;
const uint8_t HID_REQ_SET_IDLE = 0x0A;
const uint8_t HID_REQ_SET_PROTOCOL = 0x0B;
const uint16_t HID_BOOT_PROTOCOL = 0;
const uint16_t HID_REPORT_OUTPUT = 0x0200;  // wValue: tip Output, ID 0

const uint8_t HID_LED_CAPS = 1 << 1;
const uint8_t USAGE_ROLLOVER = 0x01;        // ErrorRollOver: prea multe taste apasate
const uint8_t USAGE_CAPSLOCK = 0x39;
const uint16_t MOUSE_REPORT_MAX = 8;

struct KeyDef {
    uint8_t usage;
    KeyCode code;
    char lower;
    char upper;
    bool letter;        // litera: Caps Lock inverseaza Shift
};

// HID Usage Tables, pagina 07 (Keyboard/Keypad); literele a..z sunt 0x04..0x1D si se trateaza separat
const KeyDef keys[] = {
    {0x1E, KEY_1, '1', '!', false},
    {0x1F, KEY_2, '2', '@', false},
    {0x20, KEY_3, '3', '#', false},
    {0x21, KEY_4, '4', '$', false},
    {0x22, KEY_5, '5', '%', false},
    {0x23, KEY_6, '6', '^', false},
    {0x24, KEY_7, '7', '&', false},
    {0x25, KEY_8, '8', '*', false},
    {0x26, KEY_9, '9', '(', false},
    {0x27, KEY_0, '0', ')', false},
    {0x28, KEY_ENTER, '\n', '\n', false},
    {0x29, KEY_ESC, 0, 0, false},
    {0x2A, KEY_BACKSPACE, '\b', '\b', false},
    {0x2B, KEY_TAB, '\t', '\t', false},
    {0x2C, KEY_SPACE, ' ', ' ', false},
    {0x2D, KEY_MINUS, '-', '_', false},
    {0x2E, KEY_EQUAL, '=', '+', false},
    {0x2F, KEY_LBRACKET, '[', '{', false},
    {0x30, KEY_RBRACKET, ']', '}', false},
    {0x31, KEY_BACKSLASH, '\\', '|', false},
    {0x33, KEY_SEMICOLON, ';', ':', false},
    {0x34, KEY_APOSTROPHE, '\'', '"', false},
    {0x35, KEY_GRAVE, '`', '~', false},
    {0x36, KEY_COMMA, ',', '<', false},
    {0x37, KEY_DOT, '.', '>', false},
    {0x38, KEY_SLASH, '/', '?', false},
    {0x39, KEY_CAPSLOCK, 0, 0, false},
    {0x3A, KEY_F1, 0, 0, false},
    {0x3B, KEY_F2, 0, 0, false},
    {0x3C, KEY_F3, 0, 0, false},
    {0x3D, KEY_F4, 0, 0, false},
    {0x3E, KEY_F5, 0, 0, false},
    {0x3F, KEY_F6, 0, 0, false},
    {0x40, KEY_F7, 0, 0, false},
    {0x41, KEY_F8, 0, 0, false},
    {0x42, KEY_F9, 0, 0, false},
    {0x43, KEY_F10, 0, 0, false},
    {0x44, KEY_F11, 0, 0, false},
    {0x45, KEY_F12, 0, 0, false},
    {0x49, KEY_INSERT, 0, 0, false},
    {0x4A, KEY_HOME, 0, 0, false},
    {0x4B, KEY_PGUP, 0, 0, false},
    {0x4C, KEY_DELETE, 0, 0, false},
    {0x4D, KEY_END, 0, 0, false},
    {0x4E, KEY_PGDN, 0, 0, false},
    {0x4F, KEY_RIGHT, 0, 0, false},
    {0x50, KEY_LEFT, 0, 0, false},
    {0x51, KEY_DOWN, 0, 0, false},
    {0x52, KEY_UP, 0, 0, false},
    {0x54, KEY_SLASH, '/', '?', false},     // / de pe tastatura numerica
    {0x58, KEY_ENTER, '\n', '\n', false},   // Enter de pe tastatura numerica
    {0x65, KEY_MENU, 0, 0, false},
};

// Bitii octetului de modificatori: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui
const KeyCode modifier_keys[8] = {KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LWIN, KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RWIN};
const uint8_t MODS_CTRL = 0x11;
const uint8_t MODS_SHIFT = 0x22;
const uint8_t MODS_ALT = 0x44;

// Literele a..z, generate o singura data (KeyCode nu e in ordine alfabetica)
const KeyCode letter_codes[26] = {
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

bool lookup(uint8_t usage, KeyDef& out){
    if(usage >= 0x04 && usage <= 0x1D){
        char c = 'a' + (usage - 0x04);
        out = {usage, letter_codes[usage - 0x04], c, (char)(c - 'a' + 'A'), true};
        return true;
    }
    for(const KeyDef& k : keys){
        if(k.usage == usage){
            out = k;
            return true;
        }
    }
    return false;
}

uint8_t key_mods(uint8_t report_mods, bool caps){
    uint8_t m = 0;
    if(report_mods & MODS_SHIFT){
        m |= MOD_SHIFT;
    }
    if(report_mods & MODS_CTRL){
        m |= MOD_CTRL;
    }
    if(report_mods & MODS_ALT){
        m |= MOD_ALT;
    }
    if(caps){
        m |= MOD_CAPS;
    }
    return m;
}

bool contains(const uint8_t* report, uint8_t usage){
    for(int i = 2; i < HID_KBD_REPORT; i++){
        if(report[i] == usage){
            return true;
        }
    }
    return false;
}

class HidInput final : public InputDevice {
public:
    constexpr HidInput() = default;
    InputKind kind() const override { return input_kind; }
    InputKind input_kind = InputKind::Keyboard;
    bool registered = false;
};

// O interfata HID preluata; pool static (dispozitivele HAL nu se sterg)
struct HidIface {
    HidInput hal;
    bool used = false;
    volatile bool active = false;
    bool keyboard = false;
    UsbDevice* dev = nullptr;
    UsbEndpoint* ep = nullptr;
    uint8_t iface = 0;
    uint8_t leds = 0;
    HidKbdState kbd = {};
};

const int HID_MAX = 8;
HidIface hids[HID_MAX];

bool debug_kbd = false;
bool debug_mouse = false;

// "debug=usb" sau "debug=a,usb,b" in cmdline
bool debug_has(const char* name){
    const char* v = cmdline_get("debug=");
    while(v && *v && *v != ' '){
        const char* n = name;
        const char* p = v;
        while(*n && *p == *n){
            p++;
            n++;
        }
        if(!*n && (*p == ',' || *p == ' ' || *p == '\0')){
            return true;
        }
        while(*v && *v != ',' && *v != ' '){
            v++;
        }
        if(*v == ','){
            v++;
        }
    }
    return false;
}

void push_key(const KeyEvent& k, uint8_t raw){
    InputEvent ev;
    ev.type = InputType::Key;
    ev.raw = raw;
    ev.code = 0;
    ev.value = 0;
    ev.time_ns = monotonic_ns();
    ev.key = k;
    input_push(ev);
    if(debug_kbd){
        kprintf("[usbkbd] key=%u ascii=%u pressed=%u mods=%x\n", (unsigned)k.keycode, (unsigned)(uint8_t)k.ascii,
                (unsigned)k.pressed, (unsigned)k.modifiers);
    }
}

void push_mouse(const MouseEvent& m, uint8_t raw){
    InputEvent ev;
    ev.type = InputType::Mouse;
    ev.raw = raw;
    ev.code = 0;
    ev.value = 0;
    ev.time_ns = monotonic_ns();
    ev.mouse = m;
    input_push(ev);
    if(debug_mouse){
        kprintf("[mouse] dx=%d dy=%d dz=%d buttons=%x\n", (int)m.dx, (int)m.dy, (int)m.dz, (unsigned)m.buttons);
    }
}

// Transfer interrupt terminat: decodeaza raportul si cere urmatorul
void hid_complete(UsbEndpoint* ep, int status, uint16_t length){
    HidIface* h = (HidIface*)ep->ctx;
    if(!h->active){
        return;
    }
    if(status != USB_OK){
        kprintf("[WARN] hid: transfer esuat (%d) pe portul %u, opresc interfata\n", status, h->dev->port);
        h->active = false;
        return;
    }
    if(h->keyboard){
        if(length >= HID_KBD_REPORT){
            KeyEvent evs[HID_KBD_MAX_EVENTS];
            bool leds = false;
            int n = hid_kbd_decode(h->kbd, ep->buf, evs, &leds);
            for(int i = 0; i < n; i++){
                push_key(evs[i], ep->buf[2]);
            }
            if(leds){
                h->leds = h->kbd.caps ? HID_LED_CAPS : 0;
                usb_control_async(h->dev, USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_REPORT, HID_REPORT_OUTPUT,
                                  h->iface, &h->leds, 1);
            }
        }
    } else {
        MouseEvent m;
        if(hid_mouse_decode(ep->buf, length, m)){
            push_mouse(m, ep->buf[0]);
        }
    }
    usb_submit(ep);
}

void* hid_probe(UsbDevice* dev, const UsbInterfaceDescriptor* iface, const uint8_t* extra, uint16_t len){
    if(iface->iface_protocol != HID_PROTO_KEYBOARD && iface->iface_protocol != HID_PROTO_MOUSE){
        return nullptr;
    }
    const UsbEndpointDescriptor* in = nullptr;
    for(uint16_t off = 0; off + 2 <= len && extra[off] >= 2; off += extra[off]){
        const UsbEndpointDescriptor* e = (const UsbEndpointDescriptor*)(extra + off);
        if(e->type == USB_DESC_ENDPOINT && e->length >= sizeof(UsbEndpointDescriptor) &&
           (e->address & USB_DIR_IN) && (e->attributes & 3) == USB_EP_INTERRUPT){
            in = e;
            break;
        }
    }
    bool keyboard = iface->iface_protocol == HID_PROTO_KEYBOARD;
    // Un slot liber; refolosim de preferinta unul cu acelasi tip, deja inregistrat in HAL
    HidIface* h = nullptr;
    InputKind kind = keyboard ? InputKind::Keyboard : InputKind::Mouse;
    for(HidIface& c : hids){
        if(!c.used && c.hal.registered && c.hal.input_kind == kind){
            h = &c;
            break;
        }
    }
    for(int i = 0; !h && i < HID_MAX; i++){
        if(!hids[i].used && !hids[i].hal.registered){
            h = &hids[i];
        }
    }
    if(!in || !h){
        return nullptr;
    }
    UsbEndpoint* ep = usb_add_endpoint(dev, in);
    if(!ep){
        return nullptr;
    }
    h->used = true;
    h->active = false;
    h->keyboard = keyboard;
    h->hal.input_kind = kind;
    h->dev = dev;
    h->ep = ep;
    h->iface = iface->number;
    h->leds = 0;
    memset(&h->kbd, 0, sizeof(h->kbd));
    uint16_t want = keyboard ? HID_KBD_REPORT : MOUSE_REPORT_MAX;
    ep->buf_len = ep->max_packet < want ? ep->max_packet : want;
    ep->complete = hid_complete;
    ep->ctx = h;
    return h;
}

void hid_start(UsbDevice* dev, void* data){
    HidIface* h = (HidIface*)data;
    uint8_t type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    if(usb_control(dev, type, HID_REQ_SET_PROTOCOL, HID_BOOT_PROTOCOL, h->iface, nullptr, 0) != USB_OK){
        kprintf("[WARN] hid: SET_PROTOCOL boot a esuat pe portul %u\n", dev->port);
    }
    // Rapoarte doar la schimbare (idle infinit); optional, unele mouse-uri dau STALL
    usb_control(dev, type, HID_REQ_SET_IDLE, 0, h->iface, nullptr, 0);
    if(h->keyboard){
        usb_control(dev, type, HID_REQ_SET_REPORT, HID_REPORT_OUTPUT, h->iface, &h->leds, 1);
    }
    if(!h->hal.registered){
        h->hal.registered = hal_register(HalKind::Input, &h->hal, h->keyboard ? "usb-kbd" : "usb-mouse");
    }
    h->active = true;
    usb_submit(h->ep);
    kprintf("usb: hid %s pe portul %u (interfata %u, ep %02x, %u ms)\n", h->keyboard ? "tastatura" : "mouse",
            dev->port, h->iface, h->ep->address, h->ep->interval);
}

void hid_disconnect(UsbDevice*, void* data){
    HidIface* h = (HidIface*)data;
    h->active = false;
    // Tastele ramase apasate se elibereaza, ca shell-ul sa nu ramana cu Shift tinut
    if(h->keyboard){
        uint8_t empty[HID_KBD_REPORT] = {};
        KeyEvent evs[HID_KBD_MAX_EVENTS];
        int n = hid_kbd_decode(h->kbd, empty, evs, nullptr);
        for(int i = 0; i < n; i++){
            push_key(evs[i], 0);
        }
    }
    h->used = false;
}

const UsbDriver hid_driver = {"hid-boot", HID_CLASS, HID_SUBCLASS_BOOT, 0xFF, hid_probe, hid_start, hid_disconnect};

}

uint16_t hid_usage_to_key(uint8_t usage){
    KeyDef d;
    return lookup(usage, d) ? d.code : KEY_NONE;
}

int hid_kbd_decode(HidKbdState& st, const uint8_t* r, KeyEvent* out, bool* leds_changed){
    int n = 0;
    if(leds_changed){
        *leds_changed = false;
    }
    // Modificatorii, bit cu bit
    uint8_t old_mods = st.prev[0];
    for(int b = 0; b < 8; b++){
        uint8_t bit = 1 << b;
        if((old_mods ^ r[0]) & bit){
            bool pressed = r[0] & bit;
            out[n++] = {modifier_keys[b], 0, pressed, key_mods(r[0], st.caps)};
        }
    }
    st.prev[0] = r[0];
    if(r[2] == USAGE_ROLLOVER){
        return n;
    }
    for(int i = 2; i < HID_KBD_REPORT; i++){
        uint8_t u = st.prev[i];
        KeyDef d;
        if(u > USAGE_ROLLOVER && !contains(r, u) && lookup(u, d)){
            out[n++] = {d.code, 0, false, key_mods(r[0], st.caps)};
        }
    }
    for(int i = 2; i < HID_KBD_REPORT; i++){
        uint8_t u = r[i];
        KeyDef d;
        if(u <= USAGE_ROLLOVER || contains(st.prev, u) || !lookup(u, d)){
            continue;
        }
        if(u == USAGE_CAPSLOCK){
            st.caps = !st.caps;
            if(leds_changed){
                *leds_changed = true;
            }
        }
        uint8_t mods = key_mods(r[0], st.caps);
        bool up = (mods & MOD_SHIFT) != 0;
        if(d.letter && st.caps){
            up = !up;
        }
        out[n++] = {d.code, up ? d.upper : d.lower, true, mods};
    }
    memcpy(st.prev + 1, r + 1, HID_KBD_REPORT - 1);
    return n;
}

bool hid_mouse_decode(const uint8_t* r, uint16_t len, MouseEvent& out){
    if(len < 3){
        return false;
    }
    out.buttons = r[0] & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
    out.dx = (int8_t)r[1];
    out.dy = (int8_t)r[2];                      // HID: pozitiv = jos, ca la noi
    out.dz = len >= 4 ? -(int8_t)r[3] : 0;      // HID: pozitiv = roata in sus; noi: pozitiv = spre utilizator
    return true;
}

void hid_init(){
    debug_kbd = debug_has("usb");
    debug_mouse = debug_kbd || debug_has("mouse");
    usb_register_driver(&hid_driver);
}
