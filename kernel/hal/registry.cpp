#include "hal/hal.h"
#include "hal/console.h"
#include "lib/kprintf.h"

namespace hal_detail {
HalDevice* volatile primary[(int)HalKind::Count];
}

namespace {

struct Entry {
    HalDevice* dev;
    const char* name;
};

struct Slot {
    Entry entries[HAL_MAX_PER_KIND];
    int count;
};

Slot slots[(int)HalKind::Count];

const char* const KIND_NAMES[(int)HalKind::Count] = {
    "console", "clock", "irqchip", "block", "net", "framebuffer", "input"
};

bool valid(HalKind kind){
    return (uint8_t)kind < (uint8_t)HalKind::Count;
}

}

// Inregistrarea se face la boot pe BSP; citirile sunt fara lock (count se publica dupa intrare)
bool hal_register(HalKind kind, HalDevice* dev, const char* name){
    if(!valid(kind) || !dev || !name){
        return false;
    }
    Slot& s = slots[(int)kind];
    int n = s.count;
    if(n >= HAL_MAX_PER_KIND){
        return false;
    }
    s.entries[n].dev = dev;
    s.entries[n].name = name;
    __atomic_store_n(&hal_detail::primary[(int)kind], dev, __ATOMIC_RELEASE);
    __atomic_store_n(&s.count, n + 1, __ATOMIC_RELEASE);
    return true;
}

int hal_count(HalKind kind){
    return valid(kind) ? __atomic_load_n(&slots[(int)kind].count, __ATOMIC_ACQUIRE) : 0;
}

HalDevice* hal_get(HalKind kind, int idx){
    if(idx < 0 || idx >= hal_count(kind)){
        return nullptr;
    }
    return slots[(int)kind].entries[idx].dev;
}

const char* hal_name(HalKind kind, int idx){
    if(idx < 0 || idx >= hal_count(kind)){
        return nullptr;
    }
    return slots[(int)kind].entries[idx].name;
}

HalDevice* hal_primary(HalKind kind){
    return valid(kind) ? hal_detail::primary[(int)kind] : nullptr;
}

const char* hal_kind_name(HalKind kind){
    return valid(kind) ? KIND_NAMES[(int)kind] : "?";
}

void hal_list(){
    for(int k = 0; k < (int)HalKind::Count; k++){
        HalKind kind = (HalKind)k;
        int n = hal_count(kind);
        kprintf("hal %s:", KIND_NAMES[k]);
        if(n == 0){
            kprintf(" -");
        }
        for(int i = 0; i < n; i++){
            kprintf(" %s", hal_name(kind, i));
        }
        kprintf("\n");
    }
}

Console* hal_console_screen(){
    int n = hal_count(HalKind::Console);
    for(int i = 0; i < n; i++){
        Console* c = hal_console(i);
        if(c->flags() & CONSOLE_SCREEN){
            return c;
        }
    }
    return nullptr;
}

void hal_console_putc(char c){
    int n = hal_count(HalKind::Console);
    for(int i = 0; i < n; i++){
        hal_console(i)->putc(c);
    }
}

void hal_console_set_color(uint8_t color){
    int n = hal_count(HalKind::Console);
    for(int i = 0; i < n; i++){
        hal_console(i)->set_color(color);
    }
}
