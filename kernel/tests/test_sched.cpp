#include "tests/test_sched.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "irqlock.h"

namespace {

const int THREADS = 3;
const int ROUNDS = 5;
const int MAX_LOG = THREADS * ROUNDS;
const uint64_t SPIN_NS = 25000000;          // 25 ms: mai mult decat un timeslice
const uint32_t SLEEP_MS[THREADS] = {30, 10, 20};

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
    for(int i = 0; i < THREADS; i++){
        uint64_t arg = letter_arg ? 'A' + i : i;
        Task* t = task_create_kernel(what, fn, (void*)arg);
        if(!t){
            panic("test_sched: task_create_kernel a esuat");
        }
        sched_add(t);
    }
    while(finished < THREADS){
        yield();
        __asm__ volatile("hlt");
    }
    yield();                                    // elibereaza task-urile terminate
    order[logged] = 0;
    kprintf("\n");
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

    if(task_count() != count_before){
        panic("test_sched: %lu task-uri dupa test, %lu inainte (task_exit nu a eliberat)", task_count(), count_before);
    }
    __asm__ volatile("cli");
    kprintf("[OK] test_sched (yield ABC x%d, preemptie la %u ms, sleep_ms BCA)\n", ROUNDS, SCHED_TIMESLICE_MS);
}
