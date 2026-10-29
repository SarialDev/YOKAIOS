#pragma once
#include "hal/hal.h"
#include "keycodes.h"

// Eveniment de tastatura: keycode e independent de layout, ascii e 0 pentru tastele care nu produc caracter
struct KeyEvent {
    uint16_t keycode;   // KeyCode
    char ascii;         // caracterul cu Shift/Caps aplicate, 0 daca nu e imprimabil
    bool pressed;       // true la apasare (si la autorepeat), false la eliberare
    uint8_t modifiers;  // KeyMod, starea modificatorilor dupa acest eveniment
};

// Butoane mouse (campul buttons din MouseEvent)
enum MouseButton : uint8_t {
    MOUSE_LEFT   = 1 << 0,
    MOUSE_RIGHT  = 1 << 1,
    MOUSE_MIDDLE = 1 << 2,
};

// Un pachet de mouse: deplasare relativa de la pachetul anterior + starea butoanelor
struct MouseEvent {
    int16_t dx;         // pozitiv spre dreapta
    int16_t dy;         // pozitiv in jos (coordonate de ecran; PS/2 raporteaza in sus, driverul inverseaza)
    int16_t dz;         // roata, pozitiv spre utilizator (in jos); 0 fara IntelliMouse
    uint8_t buttons;    // MouseButton, starea dupa acest pachet
};

enum class InputType : uint8_t {
    Key,        // campul key e valid
    Mouse       // campul mouse e valid
};

struct InputEvent {
    InputType type;
    uint8_t raw;        // octetul brut de la dispozitiv (tastatura: ultimul octet din secventa)
    uint16_t code;
    int32_t value;
    uint64_t time_ns;   // monotonic_ns() la primire
    KeyEvent key;       // valid doar pentru InputType::Key
    MouseEvent mouse;   // valid doar pentru InputType::Mouse
};

enum class InputKind : uint8_t { Keyboard, Mouse };

class InputDevice : public HalDevice {
public:
    virtual InputKind kind() const = 0;
protected:
    constexpr InputDevice() = default;
};

inline InputDevice* hal_input(int idx){
    return static_cast<InputDevice*>(hal_get(HalKind::Input, idx));
}

// Event bus comun tastatura/mouse: ring buffer (la plin se pierde cel mai vechi eveniment).
// Producatorii (IRQ) apeleaza input_push; consumatorii citesc cu input_pop sau se aboneaza.
const uint32_t INPUT_RING_SIZE = 128;       // putere a lui 2
// Apelat din contextul intreruperii care a produs evenimentul, dupa ce a intrat in ring
typedef void (*InputListener)(const InputEvent& ev);

void input_push(const InputEvent& ev);
bool input_pop(InputEvent& out);
// Evenimente pierdute pentru ca ring-ul era plin
uint64_t input_dropped();
// Un singur abonat; nullptr il scoate. Intoarce abonatul anterior
InputListener input_subscribe(InputListener cb);
