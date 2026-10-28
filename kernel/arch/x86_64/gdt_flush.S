/* void gdt_flush(const GdtPtr* ptr) */
.section .text
.code64
.globl gdt_flush
gdt_flush:
    lgdt (%rdi)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x28, %ax
    ltr %ax
    /* CS se reincarca printr-un far return */
    popq %rdi
    pushq $0x08
    pushq %rdi
    lretq
