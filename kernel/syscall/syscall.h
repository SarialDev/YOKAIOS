#pragma once
#include "types.h"
#include "interrupt.h"

// EFER.SCE, STAR, LSTAR, SFMASK si GS-ul per-CPU pentru intrarea din syscall.S
void syscall_init();
// Varful stivei de kernel pe care intra urmatorul syscall (la fiecare comutare de task, ca TSS.rsp0)
void syscall_set_kstack(uint64_t top);
// Apelata din syscall.S; numarul si argumentele vin din frame, rezultatul merge in f->rax
extern "C" void syscall_dispatch(InterruptFrame* f);
// Prima intrare in ring 3 (syscall.S); nu revine
extern "C" [[noreturn]] void enter_user(uint64_t rip, uint64_t rsp);

// [addr, addr + len) e sub USER_TOP si mapat in procesul curent cu USER (si RW daca write)
bool user_access_ok(uint64_t addr, uint64_t len, bool write);
// Copiaza din memoria user dupa validare; false daca intervalul nu e accesibil
bool copy_from_user(void* dst, uint64_t src, uint64_t len);

// Termina procesul curent cu codul dat (exit sau fault fatal); task-ul devine ZOMBIE
[[noreturn]] void process_exit(int code);
// Exceptie din ring 3: omoara doar procesul curent (apelata din interrupt_dispatch)
[[noreturn]] void process_kill(InterruptFrame* f);

// Codul de iesire al unui proces terminat recent (ultimele 16), pentru teste; false daca nu e gasit
bool process_exit_status(uint64_t pid, int& code);
