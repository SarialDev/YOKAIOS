/* jmp_buf: rbx rbp r12 r13 r14 r15 rsp rip */
.text
.globl setjmp
.type setjmp, @function
setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax              /* rsp dupa ret */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax               /* adresa de intoarcere */
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
.size setjmp, . - setjmp
.globl _setjmp
.set _setjmp, setjmp

.globl longjmp
.type longjmp, @function
longjmp:
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax                   /* longjmp(env, 0) intoarce 1 */
1:  movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmpq *56(%rdi)
.size longjmp, . - longjmp
.globl _longjmp
.set _longjmp, longjmp
.section .note.GNU-stack, "", @progbits
