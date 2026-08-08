
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

.global inb
.type inb, @function
inb:
    mov 4(%esp) , %dx
    in %dx , %al 
    ret 

.global outb
.type outb, @function

outb:
    mov 4(%esp) , %dx
    mov 8(%esp) , %al
    out %al , %dx
    ret

.global incarcare_idt
.type incarcare_idt, @function
incarcare_idt:
    mov 4(%esp) , %eax
    lidt (%eax)
    ret

.global tastatura_handler_asm
.extern tastatura_handler_cpp
.type tastatura_handler_asm, @function
tastatura_handler_asm:
    pusha

    call tastatura_handler_cpp

    mov $0x20, %al 
    out %al , $0x20

    popa
    iret
