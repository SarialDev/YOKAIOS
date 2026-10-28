#pragma once
#include "types.h"
#include "sched/task.h"

// Implicit: failsafe dupa 5 deadline-uri ratate la rand sau fara check-in mai mult de max(10 perioade, 50 ms)
const uint32_t WATCHDOG_DEFAULT_MISSES = 5;
const uint64_t WATCHDOG_MIN_TIMEOUT_NS = 50000000;

enum class WatchdogReason : uint8_t { DEADLINE_MISSES, NO_CHECKIN };

typedef void (*FailsafeHook)(Task* t, WatchdogReason why);

// Apelat de watchdog (din intreruperea de timer) pentru un task RT care nu mai raspunde.
// Implicit failsafe_default; se poate inlocui (ex. aterizare de urgenta pe drona)
extern FailsafeHook failsafe_hook;
// Log + oprirea task-ului
void failsafe_default(Task* t, WatchdogReason why);
const char* watchdog_reason_name(WatchdogReason why);

// Pragurile pentru un task periodic; timeout_ms 0 = implicit
void watchdog_configure(Task* t, uint32_t max_misses, uint32_t timeout_ms);
uint64_t watchdog_default_timeout_ns(uint64_t period_ns);
// Check-in explicit din job-ul curent (sfarsitul fiecarui job face check-in automat)
void watchdog_checkin();
// Din tick (1 ms), cu intreruperile oprite
void watchdog_tick(uint64_t now);
