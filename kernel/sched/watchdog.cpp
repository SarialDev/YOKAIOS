#include "sched/watchdog.h"
#include "sched/rt.h"
#include "time/clock.h"
#include "lib/kprintf.h"

FailsafeHook failsafe_hook = failsafe_default;

namespace {

void check(RtPeriodic* p, uint64_t now){
    if(p->wd_fired || p->stop){
        return;
    }
    WatchdogReason why;
    if(p->consecutive_misses >= p->wd_max_misses){
        why = WatchdogReason::DEADLINE_MISSES;
    } else if(now > p->last_checkin_ns && now - p->last_checkin_ns > p->wd_timeout_ns){
        why = WatchdogReason::NO_CHECKIN;
    } else {
        return;
    }
    p->wd_fired = true;
    failsafe_hook(p->task, why);
}

}

const char* watchdog_reason_name(WatchdogReason why){
    return why == WatchdogReason::DEADLINE_MISSES ? "deadline-uri ratate" : "fara check-in";
}

void failsafe_default(Task* t, WatchdogReason why){
    kprintf("watchdog: task %lu (%s): %s, il opresc\n", t->tid, t->name, watchdog_reason_name(why));
    rt_stop(t);
}

void watchdog_configure(Task* t, uint32_t max_misses, uint32_t timeout_ms){
    SpinlockGuard lock(rt_lock);
    RtPeriodic* p = t->rt;
    if(!p){
        return;
    }
    p->wd_max_misses = max_misses ? max_misses : WATCHDOG_DEFAULT_MISSES;
    p->wd_timeout_ns = timeout_ms ? (uint64_t)timeout_ms * 1000000 : watchdog_default_timeout_ns(p->period_ns);
}

uint64_t watchdog_default_timeout_ns(uint64_t period_ns){
    return 10 * period_ns > WATCHDOG_MIN_TIMEOUT_NS ? 10 * period_ns : WATCHDOG_MIN_TIMEOUT_NS;
}

void watchdog_checkin(){
    RtPeriodic* p = task_current()->rt;
    if(p){
        p->last_checkin_ns = monotonic_ns();
    }
}

void watchdog_tick(uint64_t now){
    rt_for_each(check, now);
}
