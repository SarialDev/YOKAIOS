#pragma once
#include "types.h"
#include "hal/input.h"

const uint8_t MOUSE_IRQ = 12;

// Pornit dupa ps2kbd_init: activeaza portul auxiliar al 8042, configureaza mouse-ul (IntelliMouse daca are roata)
// si porneste IRQ12; pachetele ajung pe event bus (hal/input.h) ca InputType::Mouse.
// Cu cmdline "debug=mouse" fiecare pachet se tipareste pe serial
void ps2mouse_init();

// Decodor de pachete, separat de hardware ca sa poata fi testat. Pachetul are 3 octeti (mouse standard)
// sau 4 (IntelliMouse, al patrulea = roata). Primul octet are mereu bitul 3 setat: ne sincronizam pe el
class Ps2MouseDecoder {
public:
    constexpr Ps2MouseDecoder() = default;
    void set_wheel(bool has_wheel) { size = has_wheel ? 4 : 3; idx = 0; }
    void reset() { idx = 0; }
    // Intoarce true cand `out` contine un pachet complet. Pachetele cu overflow se ignora, la fel octetii
    // din afara sincronizarii (primul octet fara bitul 3)
    bool feed(uint8_t byte, MouseEvent& out);
private:
    uint8_t buf[4] = {};
    uint8_t idx = 0;
    uint8_t size = 3;
};
