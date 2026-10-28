#include "sched/rt.h"
#include "sched/sched.h"
#include "sched/watchdog.h"
#include "time/clock.h"
#include "mm/kmalloc.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const int LEVELS = RT_PRIO_MAX + 1;
const uint32_t PERIOD_MAX_US = 10000000;        // 10 s
const uint64_t LATE_NS = 1000000;

static_assert(LEVELS == sizeof(RtQueue::levels) / sizeof(ListLink), "un nivel pe prioritate");

ListLink periodic = {&periodic, &periodic};
uint32_t util_ppm = 0;

Task* task_of(ListLink* l){
    return (Task*)((uint8_t*)l - __builtin_offsetof(Task, queue));
}

RtPeriodic* periodic_of(ListLink* l){
    return (RtPeriodic*)((uint8_t*)l - __builtin_offsetof(RtPeriodic, link));
}

uint8_t level_of(const Task* t){
    if(t->prio < RT_PRIO_MIN){
        return RT_PRIO_MIN;
    }
    return t->prio > RT_PRIO_MAX ? RT_PRIO_MAX : t->prio;
}

void periodic_main(void* arg){
    RtPeriodic* p = (RtPeriodic*)arg;
    while(!p->stop){
        if(p->next_release_ns > monotonic_ns()){
            sched_sleep_until(p->next_release_ns);
        }
        if(p->stop){
            break;
        }
        uint64_t release = p->next_release_ns;
        uint64_t start = monotonic_ns();
        p->fn(p->arg);
        uint64_t end = monotonic_ns();

        SpinlockGuard lock(rt_lock);
        uint64_t jitter = start - release;
        if(!p->jobs || jitter < p->jitter_min_ns){
            p->jitter_min_ns = jitter;
        }
        if(jitter > p->jitter_max_ns){
            p->jitter_max_ns = jitter;
        }
        p->jitter_sum_ns += jitter;
        if(jitter > LATE_NS){
            p->jitter_over_1ms++;
        }
        if(end - start > p->exec_max_ns){
            p->exec_max_ns = end - start;
        }
        p->jobs++;
        if(end > release + p->deadline_ns){
            p->misses++;
            p->consecutive_misses++;
        } else {
            p->consecutive_misses = 0;
        }
        p->last_checkin_ns = end;
        p->next_release_ns = release + p->period_ns;
        if(end >= p->next_release_ns + p->period_ns){
            uint64_t skipped = (end - p->next_release_ns) / p->period_ns;
            p->next_release_ns += skipped * p->period_ns;
            p->misses += skipped;
            p->consecutive_misses += (uint32_t)skipped;
        }
    }
}

}

Spinlock rt_lock("rt");

void rt_queue_init(RtQueue& q){
    for(ListLink& head : q.levels){
        head.prev = head.next = &head;
    }
    q.bitmap[0] = q.bitmap[1] = 0;
}

void rt_enqueue(RtQueue& q, Task* t){
    uint8_t lv = level_of(t);
    ListLink& head = q.levels[lv];
    ListLink& l = t->queue;
    l.prev = head.prev;
    l.next = &head;
    head.prev->next = &l;
    head.prev = &l;
    q.bitmap[lv / 64] |= 1UL << (lv % 64);
}

void rt_remove(RtQueue& q, Task* t){
    uint8_t lv = level_of(t);
    ListLink& l = t->queue;
    l.prev->next = l.next;
    l.next->prev = l.prev;
    l.prev = l.next = &l;
    if(q.levels[lv].next == &q.levels[lv]){
        q.bitmap[lv / 64] &= ~(1UL << (lv % 64));
    }
}

uint8_t rt_top_prio(const RtQueue& q){
    if(q.bitmap[1]){
        return (uint8_t)(64 + 63 - __builtin_clzl(q.bitmap[1]));
    }
    return q.bitmap[0] ? (uint8_t)(63 - __builtin_clzl(q.bitmap[0])) : 0;
}

Task* rt_pick(RtQueue& q){
    uint8_t lv = rt_top_prio(q);
    if(!lv){
        return nullptr;
    }
    Task* t = task_of(q.levels[lv].next);
    rt_remove(q, t);
    return t;
}

Task* rt_pick_if(RtQueue& q, bool (*ok)(const Task* t)){
    for(int lv = rt_top_prio(q); lv >= RT_PRIO_MIN; lv--){
        ListLink& head = q.levels[lv];
        for(ListLink* l = head.next; l != &head; l = l->next){
            Task* t = task_of(l);
            if(ok(t)){
                rt_remove(q, t);
                return t;
            }
        }
    }
    return nullptr;
}

uint8_t rt_rm_prio(uint32_t period_us){
    // 4 niveluri pe octava: 4 * log2(perioada), ca perioadele apropiate sa fie totusi ordonate
    if(period_us < 1){
        period_us = 1;
    }
    int msb = 31 - __builtin_clz(period_us);
    uint32_t frac = msb >= 2 ? (period_us >> (msb - 2)) & 3 : (period_us << (2 - msb)) & 3;
    int level = 4 * msb + (int)frac;
    return (uint8_t)(RT_PRIO_MAX - (level > RT_PRIO_MAX - RT_PRIO_MIN ? RT_PRIO_MAX - RT_PRIO_MIN : level));
}

uint32_t rt_utilization_ppm(){
    return util_ppm;
}

Task* task_create_periodic(const char* name, RtJobFn fn, uint32_t period_us, uint32_t deadline_us,
                           uint8_t prio, uint32_t wcet_us, void* arg){
    if(!fn || !period_us || period_us > PERIOD_MAX_US || !deadline_us || deadline_us > period_us ||
       !wcet_us || wcet_us > deadline_us || prio > RT_PRIO_MAX){
        return nullptr;
    }
    uint32_t need = (uint32_t)((uint64_t)wcet_us * 1000000 / period_us);
    RtPeriodic* p = (RtPeriodic*)kzalloc(sizeof(RtPeriodic));
    if(!p){
        return nullptr;
    }
    {
        SpinlockGuard lock(rt_lock);
        if(util_ppm + need > RT_UTIL_LIMIT_PPM){
            kfree(p);
            kprintf("rt: %s refuzat (U = %u + %u ppm > %u)\n", name, util_ppm, need, RT_UTIL_LIMIT_PPM);
            return nullptr;
        }
        util_ppm += need;
    }
    Task* t = task_create_kernel(name, periodic_main, p, prio ? prio : rt_rm_prio(period_us));
    if(!t){
        SpinlockGuard lock(rt_lock);
        util_ppm -= need;
        kfree(p);
        return nullptr;
    }
    uint64_t now = monotonic_ns();
    p->task = t;
    p->fn = fn;
    p->arg = arg;
    p->period_ns = (uint64_t)period_us * 1000;
    p->deadline_ns = (uint64_t)deadline_us * 1000;
    p->wcet_ns = (uint64_t)wcet_us * 1000;
    p->util_ppm = need;
    p->next_release_ns = now + p->period_ns;
    p->last_checkin_ns = now;
    p->wd_max_misses = WATCHDOG_DEFAULT_MISSES;
    p->wd_timeout_ns = watchdog_default_timeout_ns(p->period_ns);
    t->cls = TaskClass::RT;
    t->rt = p;
    {
        SpinlockGuard lock(rt_lock);
        ListLink& l = p->link;
        l.prev = periodic.prev;
        l.next = &periodic;
        periodic.prev->next = &l;
        periodic.prev = &l;
    }
    sched_add(t);
    return t;
}

void rt_stop(Task* t){
    if(t->rt){
        t->rt->stop = true;
    }
}

namespace {

// Sub rt_lock
void copy_stats(const Task* t, const RtPeriodic* p, RtStats& out){
    out.jobs = p->jobs;
    out.jitter_min_ns = p->jitter_min_ns;
    out.jitter_max_ns = p->jitter_max_ns;
    out.jitter_avg_ns = p->jobs ? p->jitter_sum_ns / p->jobs : 0;
    out.jitter_over_1ms = p->jitter_over_1ms;
    out.exec_max_ns = p->exec_max_ns;
    out.deadline_misses = p->misses;
    out.period_us = (uint32_t)(p->period_ns / 1000);
    out.prio = t->prio;
}

}

bool rt_stats(const Task* t, RtStats& out){
    SpinlockGuard lock(rt_lock);
    const RtPeriodic* p = t->rt;
    if(!p){
        return false;
    }
    copy_stats(t, p, out);
    return true;
}

void rt_stats_print(){
    SpinlockGuard lock(rt_lock);
    kprintf("rt: U = %u ppm\n", util_ppm);
    for(ListLink* l = periodic.next; l != &periodic; l = l->next){
        Task* t = periodic_of(l)->task;
        RtStats s;
        copy_stats(t, periodic_of(l), s);
        kprintf("  %s prio %u perioada %u us: %lu job-uri, jitter %lu/%lu/%lu us (min/avg/max), %lu peste 1 ms, "
                "exec max %lu us, %lu deadline-uri ratate\n",
                t->name, s.prio, s.period_us, s.jobs, s.jitter_min_ns / 1000, s.jitter_avg_ns / 1000,
                s.jitter_max_ns / 1000, s.jitter_over_1ms, s.exec_max_ns / 1000, s.deadline_misses);
    }
}

bool rt_for_each(void (*fn)(RtPeriodic* p, uint64_t now), uint64_t now){
    // Din tick: fara asteptare (si fara salvarea RFLAGS, scumpa sub emulare); daca lock-ul e ocupat se reia la tick-ul urmator
    if(!rt_lock.try_lock()){
        return false;
    }
    for(ListLink* l = periodic.next; l != &periodic; l = l->next){
        fn(periodic_of(l), now);
    }
    rt_lock.unlock();
    return true;
}

void rt_task_destroyed(Task* t){
    RtPeriodic* p = t->rt;
    if(!p){
        return;
    }
    {
        SpinlockGuard lock(rt_lock);
        p->link.prev->next = p->link.next;
        p->link.next->prev = p->link.prev;
        util_ppm -= p->util_ppm;
        t->rt = nullptr;
    }
    kfree(p);
}
