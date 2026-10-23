/* void switch_to(Context* old, Context* new)
 * Salveaza in old registrele callee-saved, rsp, rflags si adresa de revenire,
 * apoi incarca new. Un task nou are rip = task_trampoline si rsp = varful stivei.
 * Offset-uri ca in struct Context (sched/task.h). */
#define CTX_RSP    0
#define CTX_RIP    8
#define CTX_RFLAGS 16
#define CTX_RBX    24
#define CTX_RBP    32
#define CTX_R12    40
#define CTX_R13    48
#define CTX_R14    56
#define CTX_R15    64

.section .text
.code64
.globl switch_to
switch_to:
    movq %rbx, CTX_RBX(%rdi)
    movq %rbp, CTX_RBP(%rdi)
    movq %r12, CTX_R12(%rdi)
    movq %r13, CTX_R13(%rdi)
    movq %r14, CTX_R14(%rdi)
    movq %r15, CTX_R15(%rdi)
    pushfq
    popq CTX_RFLAGS(%rdi)
    leaq 1f(%rip), %rax
    movq %rax, CTX_RIP(%rdi)
    movq %rsp, CTX_RSP(%rdi)

    movq CTX_RBX(%rsi), %rbx
    movq CTX_RBP(%rsi), %rbp
    movq CTX_R12(%rsi), %r12
    movq CTX_R13(%rsi), %r13
    movq CTX_R14(%rsi), %r14
    movq CTX_R15(%rsi), %r15
    movq CTX_RSP(%rsi), %rsp
    pushq CTX_RFLAGS(%rsi)
    popfq
    jmpq *CTX_RIP(%rsi)
1:
    ret
