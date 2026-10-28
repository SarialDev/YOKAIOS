#pragma once
#include "types.h"
#include "interrupt.h"

// EFER.SCE, STAR, LSTAR, SFMASK pentru intrarea din syscall.S (GS-ul per-CPU vine din percpu_init)
void syscall_init();
// Doar MSR-urile, pe un AP
void syscall_init_cpu();
// Varful stivei de kernel pe care intra urmatorul syscall (la fiecare comutare de task, ca TSS.rsp0)
void syscall_set_kstack(uint64_t top);
// Apelata din syscall.S; numarul si argumentele vin din frame, rezultatul merge in f->rax
extern "C" void syscall_dispatch(InterruptFrame* f);
// Prima intrare in ring 3 (syscall.S); nu revine
extern "C" [[noreturn]] void enter_user(uint64_t rip, uint64_t rsp);
// Intoarcere in ring 3 cu toate registrele din f (syscall.S); nu revine
extern "C" [[noreturn]] void user_return(InterruptFrame* f);
// Frame-ul syscall-ului in curs al task-ului curent (in varful stivei lui de kernel)
InterruptFrame* syscall_frame();

// [addr, addr + len) e sub USER_TOP si mapat in procesul curent cu USER (si RW daca write)
bool user_access_ok(uint64_t addr, uint64_t len, bool write);
// Copiaza din / in memoria user dupa validare; false daca intervalul nu e accesibil
bool copy_from_user(void* dst, uint64_t src, uint64_t len);
bool copy_to_user(uint64_t dst, const void* src, uint64_t len);
// Sir terminat cu 0 din user, cu tot cu terminator in cel mult max bytes: lungimea, -EFAULT sau -E2BIG
int64_t copy_string_from_user(char* dst, uint64_t src, uint64_t max);
