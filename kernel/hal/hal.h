#pragma once
#include "types.h"

// HAL: interfete abstracte (virtual, fara RTTI) + registru de dispozitive.
// Restul kernelului vorbeste cu hardware-ul doar prin tipurile din hal/*.h
enum class HalKind : uint8_t {
    Console,
    Clock,
    IrqChip,
    Block,
    Net,
    Framebuffer,
    Input,
    Count
};

// Baza comuna; fara destructor virtual (dispozitivele sunt statice, nu se sterg cu delete)
class HalDevice {
protected:
    constexpr HalDevice() = default;
    ~HalDevice() = default;
};

const int HAL_MAX_PER_KIND = 16;

// false daca registrul pentru kind e plin sau dev/name sunt nuli; name trebuie sa fie static
bool hal_register(HalKind kind, HalDevice* dev, const char* name);
int hal_count(HalKind kind);
// idx in ordinea inregistrarii; nullptr in afara intervalului
HalDevice* hal_get(HalKind kind, int idx);
const char* hal_name(HalKind kind, int idx);
// Cel mai recent inregistrat (ex. APIC peste PIC); nullptr daca nu exista
HalDevice* hal_primary(HalKind kind);
const char* hal_kind_name(HalKind kind);
// Tipareste cu kprintf toate dispozitivele, pe tipuri
void hal_list();

namespace hal_detail {
extern HalDevice* volatile primary[(int)HalKind::Count];
}

inline HalDevice* hal_primary_fast(HalKind kind){
    return hal_detail::primary[(int)kind];
}
