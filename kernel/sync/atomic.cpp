#include "sync/atomic.h"
#include "arch/x86_64/lapic.h"

uint32_t cpu_id(){
    return lapic_enabled() ? lapic_id() : 0;
}
