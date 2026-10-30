#include "tests/test_sched.h"
#include "sched/sched.h"
#include "sched/rt.h"
#include "sched/watchdog.h"
#include "sync/mutex.h"
#include "proc/process.h"
#include "exec/elf.h"
#include "boot/multiboot2.h"
#include "arch/x86_64/percpu.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "irqlock.h"
#include "sync/atomic.h"

namespace {

const int THREADS = 3;
const int ROUNDS = 5;
const int MAX_LOG = THREADS * ROUNDS;
const uint64_t SPIN_NS = 25000000;          // 25 ms: mai mult decat un timeslice
const uint32_t SLEEP_MS[THREADS] = {30, 10, 20};

const int MUTEX_THREADS = 4;
const int MUTEX_INCREMENTS = 100000;
const uint32_t RT_PERIOD_US = 1000;             // 1000 Hz
const uint32_t RT_WCET_US = 100;
const uint64_t RT_RUN_NS = 5000000000UL;        // 5 s
const uint64_t RT_JOB_SPIN_NS = 20000;
const uint64_t JITTER_MAX_NS = 1000000;         // 1 ms
const int RT_LOAD_THREADS = 3;
// Fara "rt_strict" se tolereaza pauzele masinii gazda (QEMU fara KVM): cel mult 3% din job-uri peste 1 ms
// si 5% deadline-uri ratate
const uint64_t LENIENT_LATE_PERCENT = 3;
const uint64_t LENIENT_MISS_PERCENT = 5;
const uint64_t WAIT_TIMEOUT_NS = 90000000000UL;
const uint64_t WD_TIMEOUT_MS = 50;

char order[MAX_LOG + 1];
volatile int logged = 0;
volatile int finished = 0;

// Litera thread-ului merge si pe serial, si in "order" pentru verificare
void record(char c){
    IrqLock lock;
    kprintf("%c", c);
    if(logged < MAX_LOG){
        order[logged++] = c;
    }
}

void done(){
    IrqLock lock;
    finished++;
}

void yield_thread(void* arg){
    char c = (char)(uint64_t)arg;
    for(int i = 0; i < ROUNDS; i++){
        record(c);
        yield();
    }
    done();
}

// Nu cedeaza niciodata: alternarea vine doar din preemptia tick-ului
void spin_thread(void* arg){
    char c = (char)(uint64_t)arg;
    for(int i = 0; i < 2; i++){
        record(c);
        uint64_t end = monotonic_ns() + SPIN_NS;
        while(monotonic_ns() < end){
        }
    }
    done();
}

void sleep_thread(void* arg){
    int i = (int)(uint64_t)arg;
    uint64_t start = monotonic_ns();
    sleep_ms(SLEEP_MS[i]);
    if(monotonic_ns() - start < (uint64_t)SLEEP_MS[i] * 1000000){
        panic("test_sched: sleep_ms(%u) s-a trezit prea devreme", SLEEP_MS[i]);
    }
    record((char)('A' + i));
    done();
}

// Porneste A, B, C (in ordinea asta) si asteapta din task 0 sa se termine toate
void run(const char* what, TaskFn fn, bool letter_arg){
    logged = 0;
    finished = 0;
    kprintf("test_sched %s: ", what);
    Task* tasks[THREADS];
    for(int i = 0; i < THREADS; i++){
        uint64_t arg = letter_arg ? 'A' + i : i;
        tasks[i] = task_create_kernel(what, fn, (void*)arg);
        if(!tasks[i]){
            panic("test_sched: task_create_kernel a esuat");
        }
        // Ordinea verificata mai jos e a unui singur CPU: toate pe CPU-ul testului
        sched_set_affinity(tasks[i], (int32_t)cpu_id());
    }
    // Toate intra in coada deodata: un tick intre doua sched_add ar porni A singur
    {
        IrqLock lock;
        for(int i = 0; i < THREADS; i++){
            sched_add(tasks[i]);
        }
    }
    while(finished < THREADS){
        yield();
        __asm__ volatile("hlt");
    }
    yield();                                    // elibereaza task-urile terminate
    order[logged] = 0;
    kprintf("\n");
}

void expect(const char* what, const char* want);

// Task 0 (idle) nu se poate bloca: asteapta cu yield + hlt pana raman doar task-urile de dinainte
void wait_tasks(uint64_t count){
    uint64_t deadline = monotonic_ns() + WAIT_TIMEOUT_NS;
    while(task_count() > count){
        if(monotonic_ns() > deadline){
            panic("test_sched: %lu task-uri, asteptat %lu dupa 90 s", task_count(), count);
        }
        yield();
        __asm__ volatile("hlt");
    }
}

// Mutex: MUTEX_THREADS thread-uri incrementeaza un contor comun; fara excludere mutuala s-ar pierde incrementari
Mutex counter_mutex("sched-counter");
volatile uint64_t counter = 0;

void increment_thread(void*){
    for(int i = 0; i < MUTEX_INCREMENTS; i++){
        counter_mutex.lock();
        uint64_t v = counter;
        for(int k = 0; k < 16; k++){
            cpu_relax();                        // preemptie sau alt CPU in mijlocul sectiunii critice
        }
        counter = v + 1;
        counter_mutex.unlock();
    }
}

void test_mutex(){
    uint64_t before = task_count();
    counter = 0;
    for(int i = 0; i < MUTEX_THREADS; i++){
        Task* t = task_create_kernel("inc", increment_thread, nullptr);
        if(!t){
            panic("test_sched mutex: task_create_kernel a esuat");
        }
        sched_add(t);
    }
    wait_tasks(before);
    if(counter != (uint64_t)MUTEX_THREADS * MUTEX_INCREMENTS){
        panic("test_sched mutex: counter = %lu, asteptat %d", counter, MUTEX_THREADS * MUTEX_INCREMENTS);
    }
    kprintf("[OK] test_sched mutex (%d thread-uri x %d = %lu)\n", MUTEX_THREADS, MUTEX_INCREMENTS, counter);
}

// RT 1000 Hz sub incarcare NORMAL, toate pe acelasi CPU: se masoara preemptia NORMAL -> RT
Atomic<int> loaders_done(0);
volatile uint64_t load_end_ns = 0;

void load_thread(void*){
    while(monotonic_ns() < load_end_ns){
        for(int i = 0; i < 100; i++){
            cpu_relax();
        }
    }
    loaders_done.fetch_add(1);
}

void rt_job(void*){
    uint64_t end = monotonic_ns() + RT_JOB_SPIN_NS;
    while(monotonic_ns() < end){
        cpu_relax();
    }
}

void test_rt(){
    uint64_t before = task_count();
    loaders_done.store(0);
    load_end_ns = monotonic_ns() + RT_RUN_NS;
    int32_t cpu = (int32_t)smp_cpu_count() - 1;
    Task* rt = task_create_periodic("sched-rt", rt_job, RT_PERIOD_US, RT_PERIOD_US, 0, RT_WCET_US, nullptr);
    if(!rt){
        panic("test_sched rt: task periodic refuzat");
    }
    sched_set_affinity(rt, cpu);
    // Masuram, nu vrem ca watchdog-ul sa opreasca task-ul dupa o pauza a gazdei
    watchdog_configure(rt, 1000, 1000);
    for(int i = 0; i < RT_LOAD_THREADS; i++){
        Task* t = task_create_kernel("sched-load", load_thread, nullptr);
        if(!t){
            panic("test_sched rt: thread de incarcare");
        }
        sched_set_affinity(t, cpu);
        sched_add(t);
    }
    while(loaders_done.load() < RT_LOAD_THREADS){
        yield();
        __asm__ volatile("hlt");
    }
    RtStats s;
    if(!rt_stats(rt, s)){
        panic("test_sched rt: task-ul RT s-a oprit inainte de final (watchdog)");
    }
    rt_stop(rt);
    wait_tasks(before);

    bool strict = cmdline_has("rt_strict");
    uint64_t expected = RT_RUN_NS / (RT_PERIOD_US * 1000UL);
    kprintf("test_sched rt: %lu job-uri, jitter min/avg/max %lu/%lu/%lu us, %lu peste 1 ms, %lu deadline-uri ratate\n",
            s.jobs, s.jitter_min_ns / 1000, s.jitter_avg_ns / 1000, s.jitter_max_ns / 1000, s.jitter_over_1ms,
            s.deadline_misses);
    if(s.jobs < expected * (strict ? 99 : 90) / 100 || s.jobs > expected + 1){
        panic("test_sched rt: %lu job-uri in 5 s, asteptat ~%lu", s.jobs, expected);
    }
    if(strict){
        if(s.jitter_max_ns > JITTER_MAX_NS){
            panic("test_sched rt: jitter max %lu us > %lu us", s.jitter_max_ns / 1000, JITTER_MAX_NS / 1000);
        }
        if(s.deadline_misses){
            panic("test_sched rt: %lu deadline-uri ratate", s.deadline_misses);
        }
    } else {
        if(s.jitter_over_1ms * 100 > s.jobs * LENIENT_LATE_PERCENT){
            panic("test_sched rt: %lu job-uri din %lu cu jitter peste 1 ms", s.jitter_over_1ms, s.jobs);
        }
        if(s.deadline_misses * 100 > s.jobs * LENIENT_MISS_PERCENT){
            panic("test_sched rt: %lu deadline-uri ratate din %lu job-uri", s.deadline_misses, s.jobs);
        }
    }
    kprintf("[OK] test_sched rt (%lu job-uri la %u Hz timp de 5 s, jitter max %lu us, %lu deadline-uri ratate, %s)\n",
            s.jobs, 1000000 / RT_PERIOD_US, s.jitter_max_ns / 1000, s.deadline_misses,
            strict ? "strict" : "tolerant la pauzele gazdei");
}

// Watchdog: un task RT care se blocheaza intentionat in primul job nu mai face check-in
volatile int failsafe_calls = 0;
Task* volatile failsafe_task = nullptr;
volatile WatchdogReason failsafe_why = WatchdogReason::DEADLINE_MISSES;

// Din tick, sub rt_lock: doar noteaza; task-ul blocat il trezeste testul
void recording_hook(Task* t, WatchdogReason why){
    failsafe_task = t;
    failsafe_why = why;
    failsafe_calls++;
    failsafe_default(t, why);
}

void blocking_job(void*){
    sched_sleep_until(~0UL >> 1);               // nu se mai trezeste singur
}

void test_watchdog(){
    FailsafeHook old = failsafe_hook;
    failsafe_calls = 0;
    failsafe_task = nullptr;
    failsafe_hook = recording_hook;
    uint64_t before = task_count();
    Task* stuck = task_create_periodic("sched-stuck", blocking_job, 10000, 10000, 0, 1000, nullptr);
    if(!stuck){
        panic("test_sched watchdog: task periodic refuzat");
    }
    watchdog_configure(stuck, 0, WD_TIMEOUT_MS);
    uint64_t start = monotonic_ns();
    while(!failsafe_calls){
        if(monotonic_ns() - start > 2000000000UL){
            panic("test_sched watchdog: failsafe_hook nu a fost apelat in 2 s");
        }
        yield();
        __asm__ volatile("hlt");
    }
    uint64_t waited_ms = (monotonic_ns() - start) / 1000000;
    failsafe_hook = old;
    if(failsafe_calls != 1 || failsafe_task != stuck || failsafe_why != WatchdogReason::NO_CHECKIN){
        panic("test_sched watchdog: %d apeluri, motiv %s", failsafe_calls, watchdog_reason_name(failsafe_why));
    }
    sched_wake(stuck);                          // iese din sleep, vede stop si se termina
    wait_tasks(before);
    kprintf("[OK] test_sched watchdog (task RT blocat -> failsafe_hook dupa %lu ms, %s)\n", waited_ms,
            watchdog_reason_name(WatchdogReason::NO_CHECKIN));
}

// Programul user forktest (fork + pipe + waitpid) isi afiseaza singur "[OK] forktest"
void test_forktest(){
    static const char* const argv[] = {"forktest", nullptr};
    static const char* const envp[] = {"YOKAI=1", nullptr};
    uint64_t before = task_count();
    uint64_t pid = exec_spawn("/bin/forktest", argv, envp);
    if(!pid){
        panic("test_sched fork: forktest nu a pornit");
    }
    wait_tasks(before);
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_sched fork: forktest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_sched fork: forktest a iesit cu %d", code);
    }
    kprintf("[OK] test_sched fork (forktest pid %lu, cod de iesire 0)\n", pid);
}

void expect(const char* what, const char* want){
    for(int i = 0; want[i] || order[i]; i++){
        if(want[i] != order[i]){
            panic("test_sched %s: ordinea \"%s\", asteptat \"%s\"", what, order, want);
        }
    }
}

}

void test_sched_run(){
    __asm__ volatile("sti");
    uint64_t count_before = task_count();

    run("yield", yield_thread, true);
    expect("yield", "ABCABCABCABCABC");

    run("preemptie", spin_thread, true);
    // Fara preemptie A si-ar termina ambele runde inainte ca B sa ruleze
    if(logged != 2 * THREADS || order[0] != 'A' || order[1] != 'B' || order[2] != 'C'){
        panic("test_sched preemptie: ordinea \"%s\", asteptat \"ABC...\"", order);
    }

    run("sleep", sleep_thread, false);
    expect("sleep", "BCA");

    test_mutex();
    test_rt();
    test_watchdog();
    test_forktest();

    if(task_count() != count_before){
        panic("test_sched: %lu task-uri dupa test, %lu inainte (task_exit nu a eliberat)", task_count(), count_before);
    }
    __asm__ volatile("cli");
    kprintf("[OK] test_sched (yield ABC x%d, preemptie la %u ms, sleep_ms BCA, mutex, RT 1 kHz, watchdog, forktest)\n",
            ROUNDS, SCHED_TIMESLICE_MS);
}
