// rt_stats() (T10.2): incarcarea clasei RT vazuta din user (compozitorul reduce FPS-ul cand RT e incarcat)
#include "sched/rt.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "sync/spinlock.h"
#include "time/clock.h"
#include "arch/x86_64/percpu.h"
#include "uapi/rtstats.h"
#include "uapi/syscall_nr.h"
#include "lib/string.h"

namespace {

const uint64_t WINDOW_MIN_NS = 200000000UL;

Spinlock win_lock("rtload");
uint64_t win_start_ns = 0;          // inceputul ferestrei deschise acum
uint64_t win_start_busy = 0;
uint32_t last_load_ppm = 0;         // rezultatul ultimei ferestre inchise
uint64_t last_window_ms = 0;

}

int64_t sys_rt_stats(uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    yk_rt_stats s;
    memset(&s, 0, sizeof(s));
    uint64_t now = monotonic_ns();
    uint64_t busy = sched_rt_busy_ms();
    {
        SpinlockGuard g(win_lock);
        if(!win_start_ns){
            // Primul apel: media de la boot, apoi ferestre intre apeluri
            last_window_ms = now / 1000000UL;
            last_load_ppm = last_window_ms ? (uint32_t)(busy * 1000000UL / last_window_ms) : 0;
            win_start_ns = now;
            win_start_busy = busy;
        } else if(now - win_start_ns >= WINDOW_MIN_NS){
            last_window_ms = (now - win_start_ns) / 1000000UL;
            last_load_ppm = (uint32_t)((busy - win_start_busy) * 1000000UL / last_window_ms);
            win_start_ns = now;
            win_start_busy = busy;
        }
        s.load_ppm = last_load_ppm;
        s.window_ms = last_window_ms;
    }
    s.ncpu = smp_cpu_count();
    s.util_ppm = rt_utilization_ppm();
    rt_totals(s.periodic, s.deadline_misses);
    s.rt_busy_ms = busy;
    return copy_to_user(user, &s, sizeof(s)) ? 0 : -EFAULT;
}
