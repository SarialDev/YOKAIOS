/* Trampolina pentru AP-uri: smp.cpp o copiaza la 0x8000 (SIPI vector 0x08) si completeaza campurile tr_*.
 * AP-ul porneste in real mode cu CS:IP = 0800:0000, trece prin protected mode in long mode cu PML4-ul
 * kernelului (identity map la 0x8000), apoi sare in ap_main(cpu) pe stiva task-ului lui idle.
 * Codul ruleaza din copie, deci adresele sunt relative la 0x8000. */
#define TRAMPOLINE_BASE 0x8000
#define REL(x) ((x) - ap_trampoline_start + TRAMPOLINE_BASE)

#define CR0_PE   (1 << 0)
#define CR0_PG   (1 << 31)
#define CR4_PAE  (1 << 5)
#define MSR_EFER 0xC0000080

.section .text
.globl ap_trampoline_start
.globl ap_trampoline_end
.globl tr_cr3
.globl tr_efer
.globl tr_stack
.globl tr_entry
.globl tr_cpu

.code16
ap_trampoline_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    lgdtl REL(tr_gdt_ptr)
    movl %cr0, %eax
    orl $CR0_PE, %eax
    movl %eax, %cr0
    ljmpl $0x08, $REL(tr_pm32)

.code32
tr_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl %cr4, %eax
    orl $CR4_PAE, %eax
    movl %eax, %cr4
    movl REL(tr_cr3), %eax
    movl %eax, %cr3
    /* EFER ca pe BSP: LME, NXE (paginile kernelului au NX), SCE */
    movl $MSR_EFER, %ecx
    movl REL(tr_efer), %eax
    xorl %edx, %edx
    wrmsr
    movl %cr0, %eax
    orl $CR0_PG, %eax
    movl %eax, %cr0
    ljmpl $0x18, $REL(tr_lm64)

.code64
tr_lm64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq REL(tr_stack), %rsp
    movl REL(tr_cpu), %edi
    movq REL(tr_entry), %rax
    xorl %ebp, %ebp
    pushq $0                            /* adresa de retur falsa: aliniere ca dupa call */
    jmp *%rax

.align 16
tr_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08: cod 32-bit */
    .quad 0x00CF92000000FFFF            /* 0x10: date */
    .quad 0x00AF9A000000FFFF            /* 0x18: cod 64-bit */
tr_gdt_ptr:
    .word tr_gdt_ptr - tr_gdt - 1
    .long REL(tr_gdt)

.align 8
tr_stack:   .quad 0
tr_entry:   .quad 0
tr_cr3:     .long 0
tr_efer:    .long 0
tr_cpu:     .long 0
ap_trampoline_end:
