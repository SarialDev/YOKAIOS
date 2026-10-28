#pragma once
#include "hal/hal.h"

const uint32_t CONSOLE_SCREEN = 1;      // ecran de text cu pozitionare (VGA); serial nu are

class Console : public HalDevice {
public:
    virtual void putc(char c) = 0;
    virtual void clear() = 0;
    virtual uint32_t flags() const = 0;
    virtual void set_color(uint8_t) {}
    // Doar pentru CONSOLE_SCREEN
    virtual void put_at(const char*, int, int) {}
    virtual void set_cursor(int, int) {}
    virtual int cols() const { return 0; }
    virtual int rows() const { return 0; }
protected:
    constexpr Console() = default;
};

inline Console* hal_console(int idx){
    return static_cast<Console*>(hal_get(HalKind::Console, idx));
}

// Prima consola cu ecran de text (shell-ul scrie pe ea); nullptr daca lipseste
Console* hal_console_screen();
// Scrie pe toate consolele, in ordinea inregistrarii
void hal_console_putc(char c);
void hal_console_set_color(uint8_t color);
