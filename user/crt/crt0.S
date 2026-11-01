/* Punctul de intrare al programelor C: kernelul pune pe stiva argc, argv[], NULL, envp[], NULL, auxv.
 * __libc_start_main(main, sp) ruleaza initializarea libc, main si exit(). */
.section .text
.globl _start
.type _start, @function
_start:
    xorl %ebp, %ebp
    movq %rsp, %rsi                 /* sp -> argc */
    leaq main(%rip), %rdi
    andq $-16, %rsp
    call __libc_start_main
    ud2
.size _start, . - _start
.section .note.GNU-stack, "", @progbits
