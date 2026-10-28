#pragma once
#include "hal/hal.h"

enum class InputType : uint8_t {
    Key,        // code = scancode set 1 fara bitul 7, value = 1 apasat / 0 eliberat
    MouseRel,   // code = 0 (x), 1 (y), 2 (roata); value = deplasare cu semn
    MouseBtn    // code = indexul butonului, value = 1 / 0
};

struct InputEvent {
    InputType type;
    uint8_t raw;        // octetul brut de la dispozitiv (tastatura: scancode complet)
    uint16_t code;
    int32_t value;
    uint64_t time_ns;   // monotonic_ns() la primire
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
