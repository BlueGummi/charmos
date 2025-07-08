OUTPUT_FORMAT(elf64-x86-64)
ENTRY(k_main)

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

    .kernel_tests : { 
        __skernel_tests = .;
        KEEP(*(.kernel_tests))
        __ekernel_tests = .;
    }

    .kernel_pci_devices : { 
        __skernel_pci_devices = .;
        KEEP(*(.kernel_pci_devices))
        __ekernel_pci_devices = .;
    }

    .bss : ALIGN(4K) {
        __sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4K);
        __ebss = .;
    } :data

    __kernel_virt_end = ALIGN(4K);

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
