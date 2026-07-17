# Multiboot constants
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

# Multiboot header
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

# Stack space
.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

# Code section
.section .text
.globl _start
_start:
    movl $stack_top, %esp
    call kernel_main
halt_loop:
    cli
    hlt
    jmp halt_loop