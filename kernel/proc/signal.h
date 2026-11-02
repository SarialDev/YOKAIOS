#pragma once
#include "types.h"
#include "interrupt.h"
#include "uapi/syscall_nr.h"

struct Process;
struct AddressSpace;

// Trambulina de sigreturn: o pagina RX comuna (aceeasi pagina fizica in toate procesele), la sfarsitul spatiului user
const uint64_t SIGTRAMP_ADDR = 0x00007FFFFFFFE000UL;

// struct kernel_sigaction de pe Linux x86-64
struct KSigAction {
    uint64_t handler;               // SIG_DFL, SIG_IGN sau adresa user
    uint64_t flags;                 // SA_*
    uint64_t restorer;              // doar cu SA_RESTORER; altfel trambulina kernelului
    uint64_t mask;                  // semnale blocate cat ruleaza handlerul
};

// Starea semnalelor unui proces (in Process). Multimile au bitul (sig - 1)
struct SignalState {
    KSigAction act[NSIG];
    volatile uint64_t pending;
    uint64_t blocked;
    uint64_t saved_mask;            // sigsuspend: masca de restaurat dupa handler
    bool has_saved;
    uint64_t fault_addr;            // pentru SIGSEGV / SIGFPE / SIGILL generate de un fault
    int fault_code;
};

// Procesul curent are un semnal nebloca t de livrat? (si in kernel: pentru -EINTR)
bool signal_pending();
// Copil nou: aceleasi actiuni si masca, nimic pending
void signal_fork(Process* child, const Process* parent);
// execve: handlerele revin la SIG_DFL (SIG_IGN ramane), masca si pending se pastreaza
void signal_exec(Process* p);
// Trambulina in spatiul nou (o referinta in plus la pagina comuna); false fara memorie
bool signal_map_trampoline(AddressSpace& as);

// Trimite sig (1..31, 0 = doar verifica) procesului p; tree_lock trebuie luat. Semnalele ignorate se arunca
void signal_post_locked(Process* p, int sig);
// kill(): pid > 0 proces, grup = pgid. 0 sau -errno
int signal_send_pid(uint64_t pid, int sig);
int signal_send_pgrp(uint64_t pgid, int sig, bool check_perm = true);
// Procesul curent isi trimite singur sig (SIGPIPE)
void signal_send_self(int sig);

// La intoarcerea in ring 3 (sfarsit de syscall / IRQ / exceptie): livreaza semnalele care asteapta. Un handler modifica
// frame-ul (rip, rsp, rdi...), o actiune implicita de terminare nu mai revine. syscall_nr >= 0: syscall-ul care se
// termina (pentru SA_RESTART). Exceptie din user fara handler / blocat = proces omorat
void signal_deliver(InterruptFrame* f, int64_t syscall_nr);
// Exceptie CPU din ring 3 nerezolvata (page fault, #DE, #UD, #GP...): SIGSEGV / SIGFPE / SIGILL cu handler daca exista,
// altfel procesul moare. addr = adresa care a produs faultul
void signal_fault(InterruptFrame* f, uint64_t addr);

// Syscall-uri (signal.cpp)
int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size, uint64_t, uint64_t);
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t size, uint64_t, uint64_t);
int64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_rt_sigpending(uint64_t set, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_rt_sigsuspend(uint64_t set, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_pause(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getpgrp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getpgid(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
