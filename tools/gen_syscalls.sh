#!/bin/sh
# Genereaza stub-urile de syscall pentru libc din kernel/include/uapi/syscall_nr.h:
# fiecare "#define SYS_NUME numar" devine o functie "nume" (litere mici) care pune argumentele, face syscall si
# transforma -errno in errno + -1. Exceptii de nume: exit -> _exit, yield -> sched_yield, brk -> __brk, umount2 -> umount2 (neschimbat).
# Uz: tools/gen_syscalls.sh syscall_nr.h > syscalls.S
set -eu
echo '/* generat de tools/gen_syscalls.sh, nu se editeaza */'
cat <<'ASM'
.text
.macro SYSSTUB name, nr
.globl \name
.type \name, @function
\name:
    movl $\nr, %eax
    movq %rcx, %r10
    syscall
    cmpq $-4095, %rax
    jae __syscall_error
    ret
.size \name, . - \name
.endm
ASM
awk '/^#define SYS_[A-Z0-9_]+[ \t]+[0-9]+/ {
    n = substr($2, 5); name = tolower(n)
    if (n == "COUNT") next
    if (n == "EXIT") name = "_exit"
    if (n == "YIELD") name = "sched_yield"
    if (n == "BRK") name = "__brk"
    printf "SYSSTUB %s, %s\n", name, $3
}' "$1"
cat <<'ASM'
.globl __syscall_error
.type __syscall_error, @function
__syscall_error:
    negl %eax
    movl %eax, errno(%rip)
    movq $-1, %rax
    ret
.section .note.GNU-stack, "", @progbits
ASM
