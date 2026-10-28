#pragma once
#include "types.h"
#include "mm/vmm.h"

enum class TaskState : uint8_t { READY, RUNNING, BLOCKED, ZOMBIE };
enum class TaskClass : uint8_t { NORMAL, RT };

const size_t TASK_KSTACK_SIZE = 16384;
const int TASK_NAME_LEN = 16;

// Registrele callee-saved + rsp/rip/rflags: ce trebuie salvat la comutare (T4.2)
struct Context {
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rbx, rbp, r12, r13, r14, r15;
};

// Legaturi intruzive: un Task poate sta intr-o coada (run/wait) si in lista tuturor task-urilor
struct ListLink {
    ListLink* prev;
    ListLink* next;
};

struct RtPeriodic;
struct Process;

struct Task {
    uint64_t tid;
    uint64_t pid;               // 0 pentru thread-urile de kernel
    TaskState state;
    TaskClass cls;
    uint8_t prio;               // pentru RT: 1..99, mai mare = mai important
    Context ctx;
    uint8_t* kstack;            // baza stivei de kernel (TASK_KSTACK_SIZE bytes)
    AddressSpace* as;
    char name[TASK_NAME_LEN];
    uint32_t slice_left;        // tick-uri ramase din timeslice
    uint64_t wake_ns;           // pentru task-urile adormite (sched_sleep_until)
    ListLink queue;             // coada de rulare / asteptare
    ListLink all;               // lista tuturor task-urilor
    RtPeriodic* rt;             // task periodic (sched/rt.h), altfel nullptr
    Process* proc;              // procesul al carui thread principal e (proc/process.h), altfel nullptr
    uint32_t cpu;               // CPU-ul pe care ruleaza / in a carui coada sta / pe care a rulat ultima data
    int32_t affinity;           // -1 = orice CPU, altfel fixat pe acel CPU
    volatile bool on_cpu;       // ales de scheduler si contextul inca nesalvat (de la alegere pana dupa switch_to)
};

typedef void (*TaskFn)(void* arg);

// Creeaza cache-ul de Task si transforma thread-ul de boot in task 0 (idle)
void tasks_init();

// Thread de kernel nou, READY; porneste in fn(arg) cand il alege scheduler-ul (T4.2). nullptr fara memorie
Task* task_create_kernel(const char* name, TaskFn fn, void* arg, uint8_t prio = 0);
// Task-ul idle al unui AP (smp_init): doar stiva, AP-ul porneste direct pe ea
Task* task_create_idle(uint32_t cpu);
// Elibereaza stiva si structura unui task care nu ruleaza (ZOMBIE sau inca nepornit)
void task_destroy(Task* t);

// Task-ul care ruleaza pe CPU-ul curent (PerCpu::current)
Task* task_current();
uint64_t task_count();
