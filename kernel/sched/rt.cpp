#include "sched/rt.h"
#include "sched/sched.h"
#include "sched/watchdog.h"
#include "time/clock.h"
#include "mm/kmalloc.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "irqlock.h"

namespace {

const int LEVELS = RT_PRIO_MAX + 1;
const uint32_t PERIOD_MAX_US = 10000000;        // 10 s
const uint64_t LATE_NS = 1000000;

ListLink levels[LEVELS];
uint64_t bitmap[2];                             // bitul p = nivelul p are task-uri READY
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

void list_init(ListLink& head){
    if(!head.next){
        head.prev = head.next = &head;
    }
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

        IrqLock lock;
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

void rt_enqueue(Task* t){
    uint8_t lv = level_of(t);
    ListLink& head = levels[lv];
    list_init(head);
    ListLink& l = t->queue;
    l.prev = head.prev;
    l.next = &head;
    head.prev->next = &l;
    head.prev = &l;
    bitmap[lv / 64] |= 1UL << (lv % 64);
}

void rt_remove(Task* t){
    uint8_t lv = level_of(t);
    ListLink& l = t->queue;
    l.prev->next = l.next;
    l.next->prev = l.prev;
    l.prev = l.next = &l;
    if(levels[lv].next == &levels[lv]){
        bitmap[lv / 64] &= ~(1UL << (lv % 64));
    }
}

uint8_t rt_top_prio(){
    if(bitmap[1]){
        return (uint8_t)(64 + 63 - __builtin_clzl(bitmap[1]));
    }
    return bitmap[0] ? (uint8_t)(63 - __builtin_clzl(bitmap[0])) : 0;
}

Task* rt_pick(){
    uint8_t lv = rt_top_prio();
    if(!lv){
        return nullptr;
    }
    Task* t = task_of(levels[lv].next);
    rt_remove(t);
    return t;
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
        IrqLock lock;
        if(util_ppm + need > RT_UTIL_LIMIT_PPM){
            kfree(p);
            kprintf("rt: %s refuzat (U = %u + %u ppm > %u)\n", name, util_ppm, need, RT_UTIL_LIMIT_PPM);
            return nullptr;
        }
        util_ppm += need;
    }
    Task* t = task_create_kernel(name, periodic_main, p, prio ? prio : rt_rm_prio(period_us));
    if(!t){
        IrqLock lock;
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
        IrqLock lock;
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

bool rt_stats(const Task* t, RtStats& out){
    const RtPeriodic* p = t->rt;
    if(!p){
        return false;
    }
    IrqLock lock;
    out.jobs = p->jobs;
    out.jitter_min_ns = p->jitter_min_ns;
    out.jitter_max_ns = p->jitter_max_ns;
    out.jitter_avg_ns = p->jobs ? p->jitter_sum_ns / p->jobs : 0;
    out.jitter_over_1ms = p->jitter_over_1ms;
    out.exec_max_ns = p->exec_max_ns;
    out.deadline_misses = p->misses;
    out.period_us = (uint32_t)(p->period_ns / 1000);
    out.prio = t->prio;
    return true;
}

void rt_stats_print(){
    IrqLock lock;
    kprintf("rt: U = %u ppm\n", util_ppm);
    for(ListLink* l = periodic.next; l != &periodic; l = l->next){
        Task* t = periodic_of(l)->task;
        RtStats s;
        if(rt_stats(t, s)){
            kprintf("  %s prio %u perioada %u us: %lu job-uri, jitter %lu/%lu/%lu us (min/avg/max), %lu peste 1 ms, "
                    "exec max %lu us, %lu deadline-uri ratate\n",
                    t->name, s.prio, s.period_us, s.jobs, s.jitter_min_ns / 1000, s.jitter_avg_ns / 1000,
                    s.jitter_max_ns / 1000, s.jitter_over_1ms, s.exec_max_ns / 1000, s.deadline_misses);
        }
    }
}

void rt_for_each(void (*fn)(RtPeriodic* p, uint64_t now), uint64_t now){
    for(ListLink* l = periodic.next; l != &periodic; l = l->next){
        fn(periodic_of(l), now);
    }
}

void rt_task_destroyed(Task* t){
    RtPeriodic* p = t->rt;
    if(!p){
        return;
    }
    {
        IrqLock lock;
        p->link.prev->next = p->link.next;
        p->link.next->prev = p->link.prev;
        util_ppm -= p->util_ppm;
        t->rt = nullptr;
    }
    kfree(p);
}
