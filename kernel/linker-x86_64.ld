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
    .kernel_tests : ALIGN(64) {
        __skernel_tests = .;
        KEEP(*(.kernel_tests))
        . = ALIGN(64);
        __ekernel_tests = .;
    } :data
    .kernel_nightmare_tests : ALIGN(64) {
        __skernel_nightmare_tests = .;
        KEEP(*(.kernel_nightmare_tests))
        . = ALIGN(64);
        __ekernel_nightmare_tests = .;
    } :data
    .kernel_pci_devices : ALIGN(64) {
        __skernel_pci_devices = .;
        KEEP(*(.kernel_pci_devices))
        . = ALIGN(64);
        __ekernel_pci_devices = .;
    } :data
    .kernel_usb_drivers : ALIGN(64) {
        __skernel_usb_drivers = .;
        KEEP(*(.kernel_usb_drivers))
        . = ALIGN(64);
        __ekernel_usb_drivers = .;
    } :data
    .kernel_profiling_data : ALIGN(64) {
        __skernel_profiling_data = .;
        KEEP(*(.kernel_profiling_data))
        . = ALIGN(64);
        __ekernel_profiling_data = .;
    } :data
    .kernel_movealloc_callbacks : ALIGN(64) {
        __skernel_movealloc_callbacks = .;
        KEEP(*(.kernel_movealloc_callbacks))
        . = ALIGN(64);
        __ekernel_movealloc_callbacks = .;
    } :data
    .kernel_sched_periodic_work : ALIGN(64) {
        __skernel_sched_periodic_work = .;
        KEEP(*(.kernel_sched_periodic_work))
        . = ALIGN(64);
        __ekernel_sched_periodic_work = .;
    } :data
    .kernel_slab_sizes : ALIGN(64) {
        __skernel_slab_sizes = .;
        KEEP(*(.kernel_slab_sizes))
        . = ALIGN(64);
        __ekernel_slab_sizes = .;
    } :data
    .kernel_percpu_desc : ALIGN(64) {
        __skernel_percpu_desc = .;
        KEEP(*(.kernel_percpu_desc))
        . = ALIGN(64);
        __ekernel_percpu_desc = .;
    } :data
    .kernel_perdomain_desc : ALIGN(64) {
        __skernel_perdomain_desc = .;
        KEEP(*(.kernel_perdomain_desc))
        . = ALIGN(64);
        __ekernel_perdomain_desc = .;
    } :data
    .kernel_log_sites : ALIGN(64) {
        __skernel_log_sites = .;
        KEEP(*(.kernel_log_sites))
        . = ALIGN(64);
        __ekernel_log_sites = .;
    } :data
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
