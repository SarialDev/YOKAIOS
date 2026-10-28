#pragma once
#include "types.h"
#include "sched/task.h"

const uint32_t SCHED_TIMESLICE_MS = 10;

// Porneste scheduler-ul: task 0 (idle) ruleaza, coada e goala
void sched_init();

// Pune un task READY la coada runqueue-ului (FIFO)
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

// Apelate din tick-ul LAPIC si la iesirea din IRQ (idt.cpp)
void sched_tick();
void sched_irq_exit();

// Bucla task-ului 0: elibereaza task-urile terminate si doarme cu hlt
[[noreturn]] void sched_idle();
