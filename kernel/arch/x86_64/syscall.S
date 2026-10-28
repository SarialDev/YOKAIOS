/* Intrarea syscall si intrarea initiala in ring 3.
 * In kernel GS_BASE = &percpu si KERNEL_GS_BASE = baza user; swapgs le schimba la fiecare trecere.
 * Offset-uri ca in struct PerCpu (syscall/syscall.cpp), selectori ca in gdt.h. */
#define PERCPU_KSTACK   0
#define PERCPU_USER_RSP 8
#define SEL_UDATA       0x1B
#define SEL_UCODE       0x23
#define VEC_SYSCALL     0x80

.section .text
.code64

/* syscall: rcx = rip user, r11 = rflags user; IF/DF/TF sunt deja sterse prin SFMASK */
.globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:PERCPU_USER_RSP
    movq %gs:PERCPU_KSTACK, %rsp
    /* Frame in formatul InterruptFrame (interrupt.h) */
    pushq $SEL_UDATA
    pushq %gs:PERCPU_USER_RSP
    pushq %r11
    pushq $SEL_UCODE
    pushq %rcx
    pushq $0
    pushq $VEC_SYSCALL
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    call syscall_dispatch
    /* Fara intreruperi de aici: intre swapgs si sysret GS-ul e cel de user */
    cli
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp      /* vector + cod de eroare */
    popq %rcx           /* rip */
    addq $8, %rsp       /* cs */
    popq %r11           /* rflags */
    popq %rsp           /* rsp user */
    swapgs
    sysretq

/* void enter_user(uint64_t rip, uint64_t rsp): prima trecere in ring 3 a unui proces, cu registrele zero */
.globl enter_user
enter_user:
    cli
    pushq $SEL_UDATA
    pushq %rsi
    pushq $0x202        /* IF + bitul rezervat */
    pushq $SEL_UCODE
    pushq %rdi
    swapgs
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq
