#pragma once
#include "types.h"
#include "sched/task.h"

const uint32_t SCHED_TIMESLICE_MS = 10;
const uint64_t SCHED_NO_WAKE = ~0UL;

// Porneste scheduler-ul pe BSP: task 0 (idle) ruleaza, cozile sunt goale
void sched_init();
// Pe un AP: idle devine task-ul curent al CPU-ului
void sched_init_cpu(Task* idle);

// Pune un task READY in coada unui CPU (cel fixat prin affinity, altfel unul liber), FIFO
void sched_add(Task* t);
// Cedeaza CPU-ul urmatorului task READY; revine cand ii vine iar randul
void yield();
// Termina task-ul curent (ZOMBIE); stiva lui e eliberata de urmatorul schedule()
[[noreturn]] void task_exit();
// Blocheaza task-ul curent pana la monotonic_ns() >= wake_ns; false daca nu se poate (idle, fara tick)
bool sched_sleep_until(uint64_t wake_ns);

// Pentru WaitQueue: comuta de pe task-ul curent, deja marcat BLOCKED (cu intreruperile oprite)
void sched_block();
// Task BLOCKED -> READY, la coada runqueue-ului
void sched_wake(Task* t);

// Schimba clasa/prioritatea (priority inheritance); un task READY e mutat in coada potrivita
void sched_set_class(Task* t, TaskClass cls, uint8_t prio);
// Fixeaza t pe CPU-ul cpu (-1 = oricare) si il muta acolo; false daca CPU-ul nu e pornit
bool sched_set_affinity(Task* t, int32_t cpu);
// Cate task-uri au fost luate de un CPU liber din coada altuia (load balancing)
uint64_t sched_migrations();

// Din intreruperea de timer: trezeste task-urile adormite pana la now (inclusiv eliberarile RT)
void sched_timer_event(uint64_t now);
// Cel mai apropiat moment de trezire, SCHED_NO_WAKE daca nu doarme nimeni (pentru timer-ul one-shot)
uint64_t sched_next_wake();
// La fiecare tick de 1 ms: timeslice NORMAL si watchdog
void sched_tick(uint64_t now);
// La iesirea din IRQ, dupa EOI (idt.cpp)
void sched_irq_exit();

// Bucla task-ului idle al fiecarui CPU: elibereaza task-urile terminate, ia de lucru de la altii, doarme cu hlt
[[noreturn]] void sched_idle();
