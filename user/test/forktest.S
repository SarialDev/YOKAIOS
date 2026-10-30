/* Procese si pipe-uri: fork, copilul scrie in pipe, parintele citeste si face waitpid; apoi execve, WNOHANG
 * si un copil orfan. Orice verificare esuata iese cu un cod diferit de 0 (vezi tests/test_proc.cpp). */
#include "uapi/syscall_nr.h"

#define CHILD_CODE 7
#define WAKE_CODE  3

.macro SYS nr
    movl $\nr, %eax
    syscall
.endm

.section .text
.globl _start
_start:
    SYS SYS_GETPID
    movq %rax, %r12                 /* pid-ul parintelui */

    /* 1. pipe + fork: copilul scrie mesajul, parintele il citeste pana la EOF */
    leaq fds(%rip), %rdi
    SYS SYS_PIPE
    testq %rax, %rax
    jnz fail_pipe
    movl fds(%rip), %r13d           /* capatul de citire */
    movl fds+4(%rip), %r14d         /* capatul de scriere */

    SYS SYS_FORK
    testq %rax, %rax
    js fail_fork
    jz child_writer
    movq %rax, %rbx                 /* pid-ul copilului */

    movl %r14d, %edi                /* parintele nu scrie: altfel n-ar vedea niciodata EOF */
    SYS SYS_CLOSE
    testq %rax, %rax
    jnz fail_close
    xorl %r15d, %r15d               /* bytes cititi */
1:  movl %r13d, %edi
    leaq buf(%rip), %rsi
    addq %r15, %rsi
    movl $64, %edx
    subq %r15, %rdx
    jz 2f
    SYS SYS_READ
    testq %rax, %rax
    js fail_read
    jz 2f                           /* EOF: copilul a inchis (prin exit) capatul de scriere */
    addq %rax, %r15
    jmp 1b
2:  cmpq $msg_len, %r15
    jne fail_data
    leaq buf(%rip), %rsi
    leaq msg(%rip), %rdi
    movq %r15, %rcx
    repe cmpsb
    jne fail_data
    movl $1, %edi                   /* mesajul primit apare in log */
    leaq buf(%rip), %rsi
    movq %r15, %rdx
    SYS SYS_WRITE
    movl %r13d, %edi
    SYS SYS_CLOSE

    /* Copilul si-a modificat doar copia lui de .data */
    cmpq $0x1234, data_word(%rip)
    jne fail_copy

    movq %rbx, %rdi
    leaq status(%rip), %rsi
    xorl %edx, %edx
    xorl %r10d, %r10d
    SYS SYS_WAIT4
    cmpq %rbx, %rax
    jne fail_wait
    cmpl $(CHILD_CODE << 8), status(%rip)
    jne fail_status
    movq $-1, %rdi                  /* niciun alt copil */
    leaq status(%rip), %rsi
    xorl %edx, %edx
    xorl %r10d, %r10d
    SYS SYS_WAIT4
    cmpq $-ECHILD, %rax
    jne fail_echild

    /* 2. execve: copilul devine "hello" (care isi verifica argv si envp) */
    SYS SYS_FORK
    testq %rax, %rax
    js fail_fork
    jz child_exec
    movq %rax, %rbx
    movq %rbx, %rdi
    leaq status(%rip), %rsi
    xorl %edx, %edx
    xorl %r10d, %r10d
    SYS SYS_WAIT4
    cmpq %rbx, %rax
    jne fail_wait
    cmpl $0, status(%rip)
    jne fail_exec

    /* 3. WNOHANG: copilul e blocat in read pe un pipe gol, deci inca nu s-a terminat */
    leaq fds(%rip), %rdi
    SYS SYS_PIPE
    testq %rax, %rax
    jnz fail_pipe
    movl fds(%rip), %r13d
    movl fds+4(%rip), %r14d
    SYS SYS_FORK
    testq %rax, %rax
    js fail_fork
    jz child_reader
    movq %rax, %rbx
    SYS SYS_YIELD               /* copilul ajunge sa se blocheze in read */
    movq %rbx, %rdi
    leaq status(%rip), %rsi
    movl $WNOHANG, %edx
    xorl %r10d, %r10d
    SYS SYS_WAIT4
    testq %rax, %rax
    jnz fail_wnohang
    movl %r14d, %edi                /* un byte il trezeste */
    leaq msg(%rip), %rsi
    movl $1, %edx
    SYS SYS_WRITE
    cmpq $1, %rax
    jne fail_write
    movq %rbx, %rdi
    leaq status(%rip), %rsi
    xorl %edx, %edx
    xorl %r10d, %r10d
    SYS SYS_WAIT4
    cmpq %rbx, %rax
    jne fail_wait
    cmpl $(WAKE_CODE << 8), status(%rip)
    jne fail_status
    /* Fara cititori, scrierea da EPIPE */
    movl %r13d, %edi
    SYS SYS_CLOSE
    movl %r14d, %edi
    leaq msg(%rip), %rsi
    movl $1, %edx
    SYS SYS_WRITE
    cmpq $-EPIPE, %rax
    jne fail_epipe
    movl %r14d, %edi
    SYS SYS_CLOSE

    /* 4. Un copil care ramane orfan: parintele iese fara waitpid, kernelul il elibereaza singur */
    SYS SYS_FORK
    testq %rax, %rax
    js fail_fork
    jz child_orphan

    movl $1, %edi                   /* toate verificarile au trecut */
    leaq ok_msg(%rip), %rsi
    movl $ok_len, %edx
    SYS SYS_WRITE
    xorl %edi, %edi
    jmp do_exit

child_writer:
    movq $0x9999, data_word(%rip)
    movl %r13d, %edi
    SYS SYS_CLOSE
    SYS SYS_GETPPID
    cmpq %r12, %rax
    jne child_fail
    movl %r14d, %edi
    leaq msg(%rip), %rsi
    movl $msg_len, %edx
    SYS SYS_WRITE
    cmpq $msg_len, %rax
    jne child_fail
    movl $CHILD_CODE, %edi
    jmp do_exit

child_exec:
    leaq hello_path(%rip), %rdi
    leaq hello_argv(%rip), %rsi
    leaq hello_envp(%rip), %rdx
    SYS SYS_EXECVE
    movl $40, %edi                  /* execve a esuat */
    jmp do_exit

child_reader:
    movl %r14d, %edi
    SYS SYS_CLOSE
    movl %r13d, %edi
    leaq buf(%rip), %rsi
    movl $1, %edx
    SYS SYS_READ
    cmpq $1, %rax
    jne child_fail
    movl $WAKE_CODE, %edi
    jmp do_exit

child_orphan:
    /* Asteapta sa iasa parintele: getppid devine 0 */
    movl $100000, %ebx
1:  SYS SYS_GETPPID
    testq %rax, %rax
    jz 2f
    SYS SYS_YIELD
    decl %ebx
    jnz 1b
    jmp child_fail
2:  movl $1, %edi
    leaq orphan_msg(%rip), %rsi
    movl $orphan_len, %edx
    SYS SYS_WRITE
    xorl %edi, %edi
    jmp do_exit

child_fail:
    movl $41, %edi
    jmp do_exit

fail_pipe:    movl $1, %edi; jmp do_exit
fail_fork:    movl $2, %edi; jmp do_exit
fail_close:   movl $3, %edi; jmp do_exit
fail_read:    movl $4, %edi; jmp do_exit
fail_data:    movl $5, %edi; jmp do_exit
fail_copy:    movl $6, %edi; jmp do_exit
fail_wait:    movl $7, %edi; jmp do_exit
fail_status:  movl $8, %edi; jmp do_exit
fail_echild:  movl $9, %edi; jmp do_exit
fail_exec:    movl $10, %edi; jmp do_exit
fail_wnohang: movl $11, %edi; jmp do_exit
fail_write:   movl $12, %edi; jmp do_exit
fail_epipe:   movl $13, %edi

do_exit:
    SYS SYS_EXIT
    ud2

.section .rodata
msg:
    .ascii "mesaj prin pipe de la copil\n"
msg_len = . - msg
ok_msg:
    .ascii "[OK] forktest\n"
ok_len = . - ok_msg
orphan_msg:
    .ascii "orfan: parintele s-a terminat, getppid = 0\n"
orphan_len = . - orphan_msg
hello_path:
    .asciz "/bin/hello"
hello_arg0:
    .asciz "hello"
hello_env0:
    .asciz "YOKAI=1"

.section .data
.balign 8
hello_argv:
    .quad hello_arg0, 0
hello_envp:
    .quad hello_env0, 0
data_word:
    .quad 0x1234

.section .bss
.balign 8
fds:
    .skip 8
status:
    .skip 4
buf:
    .skip 64

.section .note.GNU-stack, "", @progbits
