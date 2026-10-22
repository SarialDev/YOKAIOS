OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS {
    . = KERNEL_VMA + 1M;
    _kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
