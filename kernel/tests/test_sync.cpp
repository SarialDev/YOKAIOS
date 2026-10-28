#include "tests/test_sync.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const int THREADS = 3;
const int INCREMENTS = 100000;
const int ITEMS = 50;
const uint8_t LOW_PRIO = 1;
const uint8_t RT_PRIO = 50;

Atomic<int> finished(0);

// Task 0 (idle) nu are voie sa se blocheze, deci asteapta cu yield + hlt
void wait_finished(int n){
    while(finished.load() < n){
        yield();
        __asm__ volatile("hlt");
    }
    yield();                                    // elibereaza task-urile terminate
    finished.store(0);
}

void start(const char* name, TaskFn fn, void* arg, TaskClass cls = TaskClass::NORMAL, uint8_t prio = 0){
    Task* t = task_create_kernel(name, fn, arg, prio);
    if(!t){
        panic("test_sync: task_create_kernel(%s) a esuat", name);
    }
    t->cls = cls;
    sched_add(t);
}

Mutex counter_mutex("counter");
volatile uint64_t counter = 0;
Atomic<uint64_t> contended(0);

// Citire si scriere separate, cu o pauza intre ele: fara mutex preemptia ar pierde incrementari
void increment_thread(void*){
    for(int i = 0; i < INCREMENTS; i++){
        if(!counter_mutex.try_lock()){
            contended.fetch_add(1);
            counter_mutex.lock();
        }
        uint64_t v = counter;
        for(int k = 0; k < 64; k++){
            cpu_relax();
        }
        counter = v + 1;
        counter_mutex.unlock();
    }
    finished.fetch_add(1);
}

void test_mutex(){
    counter = 0;
    for(int i = 0; i < THREADS; i++){
        start("inc", increment_thread, nullptr);
    }
    wait_finished(THREADS);
    if(counter != (uint64_t)THREADS * INCREMENTS){
        panic("test_mutex: counter = %lu, asteptat %d", counter, THREADS * INCREMENTS);
    }
    kprintf("[OK] test_mutex (%d thread-uri x %d = %lu, %lu lock-uri blocate)\n",
            THREADS, INCREMENTS, counter, contended.load());
}

Semaphore items(0);
Completion consumed;
volatile int received = 0;

void producer(void*){
    for(int i = 0; i < ITEMS; i++){
        items.up();
        if(i % 10 == 0){
            sleep_ms(1);
        }
    }
    finished.fetch_add(1);
}

void consumer(void*){
    for(int i = 0; i < ITEMS; i++){
        items.down();
        received++;
    }
    consumed.complete();
    finished.fetch_add(1);
}

void waiter(void*){
    consumed.wait();
    if(received != ITEMS){
        panic("test_semaphore: completion inainte de final (%d/%d)", received, ITEMS);
    }
    finished.fetch_add(1);
}

void test_semaphore(){
    start("waiter", waiter, nullptr);
    start("consumer", consumer, nullptr);
    start("producer", producer, nullptr);
    wait_finished(3);
    if(received != ITEMS || items.value() != 0 || !consumed.is_done()){
        panic("test_semaphore: primit %d/%d, semafor %u", received, ITEMS, items.value());
    }
    kprintf("[OK] test_semaphore (%d elemente producator/consumator, completion)\n", ITEMS);
}

Mutex pi_mutex("pi");
Task* volatile low_task = nullptr;
volatile uint8_t prio_while_held = 0;
volatile uint8_t prio_after_unlock = 0;

void low_thread(void*){
    low_task = task_current();
    pi_mutex.lock();
    sleep_ms(30);                               // intre timp task-ul RT incearca lock-ul
    prio_while_held = task_current()->prio;
    pi_mutex.unlock();
    prio_after_unlock = task_current()->prio;
    finished.fetch_add(1);
}

void rt_thread(void*){
    sleep_ms(5);
    pi_mutex.lock();
    pi_mutex.unlock();
    finished.fetch_add(1);
}

void test_priority_inheritance(){
    start("low", low_thread, nullptr, TaskClass::NORMAL, LOW_PRIO);
    start("rt", rt_thread, nullptr, TaskClass::RT, RT_PRIO);
    wait_finished(2);
    if(prio_while_held != RT_PRIO || prio_after_unlock != LOW_PRIO){
        panic("test_pi: prio owner %u cat timp astepta RT (asteptat %u), %u dupa unlock (asteptat %u)",
              prio_while_held, RT_PRIO, prio_after_unlock, LOW_PRIO);
    }
    kprintf("[OK] test_pi (owner ridicat la %u, revenit la %u)\n", prio_while_held, prio_after_unlock);
}

}

void test_sync_run(){
    __asm__ volatile("sti");
    uint64_t count_before = task_count();
    test_mutex();
    test_semaphore();
    test_priority_inheritance();
    if(task_count() != count_before){
        panic("test_sync: %lu task-uri ramase, %lu inainte", task_count(), count_before);
    }
    __asm__ volatile("cli");
}

void test_spin_relock(){
    static Spinlock lock("test-relock");
    lock.lock();
    lock.lock();
    panic("test_spin_relock: re-lock-ul nu a fost detectat");
}
