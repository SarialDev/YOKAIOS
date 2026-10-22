/* 256 stub-uri de intrerupere si calea comuna catre interrupt_dispatch */
.section .text
.code64

.macro ISR_NOERR n
isr_stub_\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
isr_stub_\n:
    pushq $\n
    jmp isr_common
.endm

isr_common:
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
    cld
    call interrupt_dispatch
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
    iretq

/* Vectorii la care CPU-ul pune singur cod de eroare */
.altmacro
.macro ISR n
    .if (\n == 8) || (\n == 10) || (\n == 11) || (\n == 12) || (\n == 13) || (\n == 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
        ISR_ERR \n
    .else
        ISR_NOERR \n
    .endif
.endm

.set i, 0
.rept 256
    ISR %i
    .set i, i + 1
.endr

.macro STUB_ADDR n
    .quad isr_stub_\n
.endm

.section .rodata
.align 8
.globl isr_stub_table
isr_stub_table:
.set i, 0
.rept 256
    STUB_ADDR %i
    .set i, i + 1
.endr
