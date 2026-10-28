/* Primul program user: verifica stiva initiala, segmentele si syscall-urile, apoi exit(0).
 * Orice verificare esuata iese cu un cod diferit de 0 (vezi tests/test_user.cpp). */
#include "uapi/syscall_nr.h"

#define AT_NULL   0
#define AT_PAGESZ 6

.section .text
.globl _start
_start:
    /* Stiva: argc, argv[], NULL, envp[], NULL, auxv (perechi tip/valoare) */
    movq (%rsp), %rax
    cmpq $1, %rax
    jne fail_args
    movq 8(%rsp), %rax
    cmpb $0x68, (%rax)              /* argv[0] incepe cu 'h' */
    jne fail_args
    cmpq $0, 16(%rsp)
    jne fail_args
    leaq 24(%rsp), %rsi
    movq (%rsi), %rax               /* envp[0] = "YOKAI=1" */
    testq %rax, %rax
    jz fail_args
    cmpb $0x59, (%rax)
    jne fail_args
1:  movq (%rsi), %rax
    addq $8, %rsi
    testq %rax, %rax
    jnz 1b
2:  movq (%rsi), %rax               /* auxv: cauta AT_PAGESZ */
    cmpq $AT_NULL, %rax
    je fail_auxv
    cmpq $AT_PAGESZ, %rax
    je 3f
    addq $16, %rsi
    jmp 2b
3:  cmpq $4096, 8(%rsi)
    jne fail_auxv
    testq $15, %rsp
    jnz fail_auxv

    /* .data are valoarea din fisier, .bss e zero si se poate scrie */
    cmpq $0x1234, data_word(%rip)
    jne fail_mem
    cmpq $0, bss_word(%rip)
    jne fail_mem
    movq $1, bss_word(%rip)

    /* Registrele callee-saved trebuie sa supravietuiasca syscall-urilor */
    movabsq $0x1111222233334444, %r12
    movabsq $0x5555666677778888, %r13
    movq %rsp, %r14

    movl $SYS_WRITE, %eax
    movl $1, %edi
    leaq msg(%rip), %rsi
    movl $msg_len, %edx
    syscall
    cmpq $msg_len, %rax
    jne fail_write

    /* Bucla lunga in ring 3: tick-ul intrerupe codul user (swapgs pe calea de intrerupere) */
    movl $20000000, %ecx
4:  decl %ecx
    jnz 4b

    /* Pointer de kernel, pointer nemapat, fd invalid, numar necunoscut */
    movl $SYS_WRITE, %eax
    movl $1, %edi
    movabsq $0xFFFFFFFF80100000, %rsi
    movl $4, %edx
    syscall
    cmpq $-EFAULT, %rax
    jne fail_efault
    movl $SYS_WRITE, %eax
    movl $1, %edi
    movl $0x1000, %esi
    movl $4, %edx
    syscall
    cmpq $-EFAULT, %rax
    jne fail_efault
    movl $SYS_WRITE, %eax
    movl $5, %edi
    leaq msg(%rip), %rsi
    movl $msg_len, %edx
    syscall
    cmpq $-EBADF, %rax
    jne fail_ebadf
    movl $999, %eax
    syscall
    cmpq $-ENOSYS, %rax
    jne fail_enosys

    movl $SYS_GETPID, %eax
    syscall
    testq %rax, %rax
    jz fail_pid
    movq %rax, %r15
    movl $SYS_YIELD, %eax
    syscall
    testq %rax, %rax
    jnz fail_yield
    movl $SYS_GETPID, %eax
    syscall
    cmpq %rax, %r15
    jne fail_pid

    movabsq $0x1111222233334444, %rax
    cmpq %rax, %r12
    jne fail_regs
    movabsq $0x5555666677778888, %rax
    cmpq %rax, %r13
    jne fail_regs
    cmpq %rsp, %r14
    jne fail_regs

    xorl %edi, %edi
    jmp do_exit

fail_args:   movl $1, %edi; jmp do_exit
fail_auxv:   movl $2, %edi; jmp do_exit
fail_mem:    movl $3, %edi; jmp do_exit
fail_write:  movl $4, %edi; jmp do_exit
fail_efault: movl $5, %edi; jmp do_exit
fail_ebadf:  movl $6, %edi; jmp do_exit
fail_enosys: movl $7, %edi; jmp do_exit
fail_pid:    movl $8, %edi; jmp do_exit
fail_yield:  movl $9, %edi; jmp do_exit
fail_regs:   movl $10, %edi

do_exit:
    movl $SYS_EXIT, %eax
    syscall
    ud2                             /* exit nu revine */

.section .rodata
msg:
    .ascii "hello from ring 3\n"
msg_len = . - msg

.section .data
data_word:
    .quad 0x1234

.section .bss
bss_word:
    .quad 0

.section .note.GNU-stack, "", @progbits
