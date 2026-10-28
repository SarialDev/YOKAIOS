/* Program care face intentionat o exceptie in ring 3; kernelul trebuie sa omoare doar procesul.
 * argv[1][0] alege exceptia. Daca exceptia nu vine, iese cu 1. */
#include "uapi/syscall_nr.h"

.section .text
.globl _start
_start:
    movq (%rsp), %rax
    cmpq $2, %rax
    jne no_fault
    movq 16(%rsp), %rax
    movzbl (%rax), %eax
    cmpb $0x70, %al                 /* 'p': scriere la adresa 0 (nemapata) */
    je write_null
    cmpb $0x6B, %al                 /* 'k': citire din kernel (pagina fara USER) */
    je read_kernel
    cmpb $0x67, %al                 /* 'g': instructiune privilegiata -> #GP */
    je privileged
    cmpb $0x77, %al                 /* 'w': scriere in .rodata (pagina fara RW) */
    je write_rodata
    cmpb $0x78, %al                 /* 'x': executie din .data (pagina NX) */
    je exec_data
    jmp no_fault

write_null:
    movq $1, 0
    jmp no_fault
read_kernel:
    movabsq $0xFFFFFFFF80100000, %rax
    movq (%rax), %rax
    jmp no_fault
privileged:
    cli
    jmp no_fault
write_rodata:
    movq $1, ro_word(%rip)
    jmp no_fault
exec_data:
    leaq data_code(%rip), %rax
    call *%rax
no_fault:
    movl $SYS_EXIT, %eax
    movl $1, %edi
    syscall
    ud2

.section .rodata
ro_word:
    .quad 0

.section .data
data_code:
    ret

.section .note.GNU-stack, "", @progbits
