#pragma once
#include "types.h"
#include "sched/task.h"

// Clasa RT: prioritate fixa 1..99 (mai mare = mai important), mereu inaintea task-urilor NORMAL
const uint8_t RT_PRIO_MIN = 1;
const uint8_t RT_PRIO_MAX = 99;
// Admission test pentru rate-monotonic: suma wcet/perioada <= 0.69 (in milionimi)
const uint32_t RT_UTIL_LIMIT_PPM = 690000;

// Coada de rulare RT: o lista FIFO pe nivel si un bitmap de niveluri ocupate (alegere O(1)).
// Se apeleaza doar din scheduler, cu intreruperile oprite
void rt_enqueue(Task* t);
void rt_remove(Task* t);
Task* rt_pick();
// Prioritatea cea mai mare cu task-uri READY, 0 daca nu e niciunul
uint8_t rt_top_prio();

typedef void (*RtJobFn)(void* arg);

// Starea unui task periodic (Task::rt); actualizata de task la fiecare job, citita de watchdog
struct RtPeriodic {
    Task* task;
    RtJobFn fn;
    void* arg;
    uint64_t period_ns;
    uint64_t deadline_ns;           // relativ la eliberarea job-ului
    uint64_t wcet_ns;               // folosit doar de admission test
    uint32_t util_ppm;
    uint64_t next_release_ns;
    uint64_t jobs;
    uint64_t jitter_min_ns;         // jitter = inceputul job-ului - momentul eliberarii
    uint64_t jitter_max_ns;
    uint64_t jitter_sum_ns;
    uint64_t jitter_over_1ms;       // job-uri pornite cu peste 1 ms intarziere
    uint64_t exec_max_ns;
    uint64_t misses;                // job-uri terminate dupa deadline + eliberari sarite
    uint32_t consecutive_misses;
    uint64_t last_checkin_ns;
    uint32_t wd_max_misses;         // watchdog: deadline-uri ratate la rand
    uint64_t wd_timeout_ns;         // watchdog: timp maxim fara check-in
    bool wd_fired;
    volatile bool stop;             // task-ul iese la urmatorul punct sigur
    ListLink link;
};

struct RtStats {
    uint64_t jobs;
    uint64_t jitter_min_ns;
    uint64_t jitter_max_ns;
    uint64_t jitter_avg_ns;
    uint64_t jitter_over_1ms;
    uint64_t exec_max_ns;
    uint64_t deadline_misses;
    uint32_t period_us;
    uint8_t prio;
};

// Task RT periodic: fn(arg) ruleaza o data la fiecare perioada, cu deadline relativ la eliberare.
// Daca un job intarzie peste o perioada intreaga, eliberarile trecute se sar (numarate ca ratate),
// in loc sa ruleze toate una dupa alta.
// prio 0 = rate-monotonic (perioada mica = prioritate mare). wcet_us intra in admission test.
// nullptr daca utilizarea totala ar depasi 0.69 sau parametrii sunt invalizi
Task* task_create_periodic(const char* name, RtJobFn fn, uint32_t period_us, uint32_t deadline_us,
                           uint8_t prio, uint32_t wcet_us, void* arg = nullptr);
// Prioritatea rate-monotonic pentru o perioada
uint8_t rt_rm_prio(uint32_t period_us);
// Utilizarea totala admisa pana acum (milionimi)
uint32_t rt_utilization_ppm();
// Cere oprirea unui task periodic: iese la urmatoarea eliberare, sau la iesirea din IRQ daca ruleaza
void rt_stop(Task* t);
// Copie a statisticilor; false daca t nu e periodic
bool rt_stats(const Task* t, RtStats& out);
void rt_stats_print();

// Pentru watchdog: fn(p, now) pe fiecare task periodic, cu intreruperile oprite
void rt_for_each(void (*fn)(RtPeriodic* p, uint64_t now), uint64_t now);
// Din task_destroy: scoate task-ul din lista si ii elibereaza utilizarea
void rt_task_destroyed(Task* t);
