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

    /* Cod doar pentru boot: eliberat dupa init, deci pe pagini proprii */
    .init.text ALIGN(4K) : AT(ADDR(.init.text) - KERNEL_VMA) {
        _init_start = .;
        *(.init.text)
        . = ALIGN(4K);
        _init_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        _rodata_start = .;
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        _data_start = .;
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;

    /* Tabelul de relocari KASLR (tools/kaslr_relocs.py), dupa _kernel_end: doar kaslr_setup il citeste, la boot,
     * iar marimea lui nu muta nimic din imagine (link in doi pasi, vezi Makefile) */
    .kaslr_relocs ALIGN(4K) : AT(ADDR(.kaslr_relocs) - KERNEL_VMA) {
        __kaslr_relocs = .;
        KEEP(*(.kaslr_relocs))
        __kaslr_relocs_end = .;
    }

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
