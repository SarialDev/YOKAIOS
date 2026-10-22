#define KERNEL_VMA 0xFFFFFFFF80000000
#define PHYS(x) ((x) - KERNEL_VMA)

#define MB2_MAGIC  0xE85250D6
#define MB2_LOADER 0x36D76289

/* Multiboot2 header */
.section .multiboot, "a"
.align 8
mb2_start:
    .long MB2_MAGIC
    .long 0
    .long mb2_end - mb2_start
    .long -(MB2_MAGIC + (mb2_end - mb2_start))
    /* entry address tag: GRUB jumps to the physical entry */
    .align 8
    .short 3, 0
    .long 12
    .long PHYS(_start)
    /* framebuffer tag comes with the framebuffer console (T5.5); with it GRUB leaves VGA text mode */
    /* end tag */
    .align 8
    .short 0, 0
    .long 8
mb2_end:

/* Boot page tables: 0-1 GiB identity and at KERNEL_VMA, 2 MiB pages */
.section .data
.align 4096
.globl boot_pml4, boot_pdpt
boot_pml4:
    .quad PHYS(boot_pdpt) + 3
    .fill 510, 8, 0
    .quad PHYS(boot_pdpt) + 3
boot_pdpt:
    .quad PHYS(boot_pd) + 3
    .fill 509, 8, 0
    .quad PHYS(boot_pd) + 3
    .quad 0                             /* MMIO window, filled by mmio_map */
boot_pd:
    .set i, 0
    .rept 512
    .quad (i << 21) + 0x83
    .set i, i + 1
    .endr

.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF    /* 0x08 code64 */
    .quad 0x00CF92000000FFFF    /* 0x10 data */
gdt64_end:
gdt64_ptr_phys:
    .short gdt64_end - gdt64 - 1
    .long PHYS(gdt64)
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .quad gdt64

.section .bss
.align 16
stack_bottom:
    .skip 16384
.globl stack_top
stack_top:

.section .text
.code32
.globl _start
_start:
    cli
    movl $PHYS(stack_top), %esp
    movl %eax, %edi
    movl %ebx, %esi

    cmpl $MB2_LOADER, %edi
    jne no_lm
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_lm
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz no_lm

    movl %cr4, %eax
    orl $(1 << 5), %eax                 /* PAE */
    movl %eax, %cr4
    movl $PHYS(boot_pml4), %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx              /* EFER */
    rdmsr
    orl $(1 << 8), %eax                 /* LME */
    wrmsr
    movl %cr0, %eax
    orl $(1 << 31), %eax                /* PG */
    movl %eax, %cr0

    lgdt PHYS(gdt64_ptr_phys)
    ljmp $0x08, $PHYS(long_mode_low)

no_lm:
    movl $0xB8000, %ebx
    movw $0x4F4E, (%ebx)                /* N */
    movw $0x4F4F, 2(%ebx)               /* O */
    movw $0x4F20, 4(%ebx)               /* ' ' */
    movw $0x4F4C, 6(%ebx)               /* L */
    movw $0x4F4D, 8(%ebx)               /* M */
1:  hlt
    jmp 1b

.code64
long_mode_low:
    movabsq $long_mode_high, %rax
    jmp *%rax

long_mode_high:
    lgdt gdt64_ptr
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movabsq $stack_top, %rsp

    /* drop the low identity map */
    movabsq $boot_pml4, %rax
    movq $0, (%rax)
    movq %cr3, %rax
    movq %rax, %cr3

    movl %edi, %edi                     /* magic */
    movl %esi, %esi                     /* mbi physical */
    xorq %rbp, %rbp                     /* end of RBP chain for stack traces */
    call kernel_main
2:  cli
    hlt
    jmp 2b
