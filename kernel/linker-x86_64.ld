OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS {
    limine_requests PT_LOAD FLAGS((1 << 1) | (1 << 2));
    text PT_LOAD FLAGS((1 << 0) | (1 << 2));
    rodata PT_LOAD FLAGS(1 << 2);
    data PT_LOAD FLAGS((1 << 1) | (1 << 2));
}

SECTIONS {
    . = 0xffffffff80000000;

    .limine_requests : ALIGN(4K) {
        __slimine_requests = .;
        *(.limine_requests .limine_requests*)
        . = ALIGN(4K);
        __elimine_requests = .;
    } :limine_requests

    .text : ALIGN(4K) {
        __stext = .;
        *(.text .text.*)
        . = ALIGN(4K);
        __etext = .;
    } :text

    .rodata : ALIGN(4K) {
        __srodata = .;
        *(.rodata .rodata.*)
        . = ALIGN(4K);
        __erodata = .;
    } :rodata

    .data : ALIGN(4K) {
        __sdata = .;
        *(.data .data.*)
        . = ALIGN(4K);
        __edata = .;
    } :data

    .bss : ALIGN(4K) {
        __sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4K);
        __ebss = .;
    } :data

    __kernel_virt_start = 0xffffffff80000000;
    __kernel_virt_end = ALIGN(4K);
    __kernel_phys_start = __kernel_virt_start - 0xffffffff80000000;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
