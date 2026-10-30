#include "tests/test_rt.h"
#include "sched/rt.h"
#include "sched/sched.h"
#include "sched/watchdog.h"
#include "time/clock.h"
#include "sync/atomic.h"
#include "boot/multiboot2.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "arch/x86_64/percpu.h"

namespace {

const uint32_t RT_PERIOD_US = 1000;             // 1000 Hz
const uint32_t RT_WCET_US = 100;
const uint64_t RUN_NS = 10000000000UL;          // 10 s
const uint64_t JITTER_MAX_NS = 1000000;         // 1 ms
const int LOAD_THREADS = 3;
const uint64_t JOB_SPIN_NS = 20000;
// Fara "rt_strict" (make test) se tolereaza pauzele masinii gazda: cel mult 3% din job-uri peste 1 ms
const uint64_t LENIENT_LATE_PERCENT = 3;

Atomic<int> loaders_done(0);
Atomic<uint64_t> load_loops(0);
volatile uint64_t load_end_ns = 0;
volatile uint64_t rt_jobs_seen = 0;

uint64_t failsafe_tid = 0;
WatchdogReason failsafe_why = WatchdogReason::DEADLINE_MISSES;
int failsafe_calls = 0;

void spin_ns(uint64_t ns){
    uint64_t end = monotonic_ns() + ns;
    while(monotonic_ns() < end){
        cpu_relax();
    }
}

// Task 0 (idle) nu se poate bloca: asteapta cu yield + hlt pana raman doar task-urile de dinainte
void wait_tasks(uint64_t count){
    while(task_count() > count){
        yield();
        __asm__ volatile("hlt");
    }
}

// Thread NORMAL care tine CPU-ul ocupat pana la load_end_ns
void load_thread(void*){
    while(monotonic_ns() < load_end_ns){
        load_loops.fetch_add(1);
        for(int i = 0; i < 100; i++){
            cpu_relax();
        }
    }
    loaders_done.fetch_add(1);
}

void rt_job(void*){
    rt_jobs_seen++;
    spin_ns(JOB_SPIN_NS);
}

void slow_job(void*){
    spin_ns(3000000);                           // 3 ms > deadline de 2 ms
}

void runaway_job(void*){
    for(;;){
        cpu_relax();                            // nu se termina niciodata: doar watchdog-ul il opreste
    }
}

void recording_hook(Task* t, WatchdogReason why){
    failsafe_tid = t->tid;
    failsafe_why = why;
    failsafe_calls++;
    failsafe_default(t, why);
}

Task* must_create(const char* name, RtJobFn fn, uint32_t period, uint32_t deadline, uint8_t prio, uint32_t wcet){
    Task* t = task_create_periodic(name, fn, period, deadline, prio, wcet);
    if(!t){
        panic("test_rt: %s nu a fost admis", name);
    }
    return t;
}

void test_admission(){
    if(rt_rm_prio(1000) <= rt_rm_prio(5000) || rt_rm_prio(5000) <= rt_rm_prio(100000)){
        panic("test_rt: prioritatile rate-monotonic nu scad cu perioada (%u, %u, %u)",
              rt_rm_prio(1000), rt_rm_prio(5000), rt_rm_prio(100000));
    }
    // 0.6 + 0.1 = 0.7 > 0.69: al doilea trebuie refuzat, fara sa ramana utilizare rezervata
    uint64_t before = task_count();
    Task* a = must_create("adm-a", rt_job, 10000, 10000, 0, 6000);
    if(task_create_periodic("adm-b", rt_job, 10000, 10000, 0, 1000)){
        panic("test_rt: admission test a acceptat U = 0.7");
    }
    if(rt_utilization_ppm() != 600000){
        panic("test_rt: utilizare %u ppm dupa refuz, asteptat 600000", rt_utilization_ppm());
    }
    rt_stop(a);
    wait_tasks(before);
    if(rt_utilization_ppm() != 0){
        panic("test_rt: utilizare %u ppm dupa oprire", rt_utilization_ppm());
    }
}

void test_jitter(){
    uint64_t before = task_count();
    load_end_ns = monotonic_ns() + RUN_NS;
    // Toate pe acelasi CPU: se masoara preemptia NORMAL -> RT, nu cate CPU-uri libere are gazda
    int32_t cpu = (int32_t)smp_cpu_count() - 1;
    Task* rt = must_create("rt-1khz", rt_job, RT_PERIOD_US, RT_PERIOD_US, 0, RT_WCET_US);
    sched_set_affinity(rt, cpu);
    // Masuram jitter-ul, nu vrem ca watchdog-ul sa opreasca task-ul dupa o pauza a gazdei
    watchdog_configure(rt, 1000, 1000);
    for(int i = 0; i < LOAD_THREADS; i++){
        Task* t = task_create_kernel("load", load_thread, nullptr);
        if(!t){
            panic("test_rt: thread de incarcare");
        }
        sched_set_affinity(t, cpu);
        sched_add(t);
    }
    // Task 0 ajunge sa ruleze abia cand thread-urile NORMAL au terminat cele 10 s
    while(loaders_done.load() < LOAD_THREADS){
        yield();
        __asm__ volatile("hlt");
    }
    RtStats s;
    if(!rt_stats(rt, s)){
        panic("test_rt: rt-1khz s-a oprit inainte de final (watchdog)");
    }
    rt_stats_print();
    rt_stop(rt);
    wait_tasks(before);

    bool strict = cmdline_has("rt_strict");
    uint64_t expected = RUN_NS / (RT_PERIOD_US * 1000UL);
    if(s.jobs < expected * (strict ? 99 : 90) / 100 || s.jobs > expected + 1){
        panic("test_rt: %lu job-uri in 10 s, asteptat ~%lu", s.jobs, expected);
    }
    if(strict && s.jitter_max_ns > JITTER_MAX_NS){
        panic("test_rt: jitter max %lu us > %lu us", s.jitter_max_ns / 1000, JITTER_MAX_NS / 1000);
    }
    if(!strict && s.jitter_over_1ms * 100 > s.jobs * LENIENT_LATE_PERCENT){
        panic("test_rt: %lu job-uri din %lu cu jitter peste 1 ms", s.jitter_over_1ms, s.jobs);
    }
    if(!load_loops.load()){
        panic("test_rt: thread-urile NORMAL nu au rulat");
    }
    kprintf("[OK] test_rt jitter%s (%lu job-uri la %u Hz cu %d thread-uri NORMAL, jitter min/avg/max %lu/%lu/%lu us, "
            "%lu peste 1 ms, %lu deadline-uri ratate)\n", strict ? " strict" : "", s.jobs, 1000000 / RT_PERIOD_US,
            LOAD_THREADS, s.jitter_min_ns / 1000, s.jitter_avg_ns / 1000, s.jitter_max_ns / 1000, s.jitter_over_1ms,
            s.deadline_misses);
}

void expect_failsafe(Task* t, uint64_t before, WatchdogReason why, const char* what){
    uint64_t tid = t->tid;
    wait_tasks(before);
    if(failsafe_calls != 1 || failsafe_tid != tid || failsafe_why != why){
        panic("test_rt: watchdog %s: %d apeluri, tid %lu (asteptat %lu), motiv %s",
              what, failsafe_calls, failsafe_tid, tid, watchdog_reason_name(failsafe_why));
    }
    failsafe_calls = 0;
}

void test_watchdog(){
    FailsafeHook old = failsafe_hook;
    failsafe_hook = recording_hook;

    // Fiecare job depaseste deadline-ul: dupa 5 la rand, failsafe
    uint64_t before = task_count();
    Task* slow = must_create("wd-slow", slow_job, 10000, 2000, 0, 1000);
    expect_failsafe(slow, before, WatchdogReason::DEADLINE_MISSES, "deadline");

    // Bucla infinita la prioritate maxima: task 0 nu mai ruleaza, doar timeout-ul de check-in o opreste
    Task* stuck = must_create("wd-stuck", runaway_job, 10000, 10000, RT_PRIO_MAX, 1000);
    watchdog_configure(stuck, 0, 50);
    expect_failsafe(stuck, before, WatchdogReason::NO_CHECKIN, "check-in");

    failsafe_hook = old;
    kprintf("[OK] test_watchdog (5 deadline-uri ratate -> oprit, bucla infinita RT -> oprita dupa 50 ms)\n");
}

}

void test_rt_run(){
    __asm__ volatile("sti");
    if(!clock_high_res()){
        kprintf("test_rt: timer periodic (fara TSC), jitter-ul poate ajunge la 1 ms\n");
    }
    test_admission();
    test_watchdog();
    test_jitter();
    if(rt_utilization_ppm() != 0){
        panic("test_rt: utilizare %u ppm ramasa", rt_utilization_ppm());
    }
    __asm__ volatile("cli");
    kprintf("[OK] test_rt (admission U <= 0.69, rate-monotonic)\n");
}
