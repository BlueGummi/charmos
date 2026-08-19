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
    __kernel_virt_start = ALIGN(4K);

    /* Limine */
    .limine_requests : ALIGN(4K) {
        __slimine_requests = .;
        *(.limine_requests .limine_requests*)
        . = ALIGN(4K);
        __elimine_requests = .;
    } :limine_requests

    /* Basic kernel sections */
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

    /* Custom kernel sections */
    .kernel_tests : ALIGN(64) {
        __skernel_tests = .;
        KEEP(*(.kernel_tests))
        __ekernel_tests = .;
        . = ALIGN(64);
    } :data
    .kernel_test_groups : ALIGN(64) {
        __skernel_test_groups = .;
        KEEP(*(.kernel_test_groups))
        __ekernel_test_groups = .;
        . = ALIGN(64);
    } :data
    .kernel_nightmare_test_groups : ALIGN(64) {
        __skernel_nightmare_tests = .;
        KEEP(*(.kernel_nightmare_tests))
        __ekernel_nightmare_tests = .;
        . = ALIGN(64);
    } :data
    .kernel_test_imports : ALIGN(64) {
        __skernel_test_imports = .;
        KEEP(*(.kernel_test_imports))
        __ekernel_test_imports = .;
        . = ALIGN(64);
    } :data
    .kernel_test_exports : ALIGN(64) {
        __skernel_test_exports = .;
        KEEP(*(.kernel_test_exports))
        __ekernel_test_exports = .;
        . = ALIGN(64);
    } :data
    .kernel_test_canonical_signatures : ALIGN(64) {
        __skernel_test_canonical_signatures = .;
        KEEP(*(.kernel_test_canonical_signatures))
        __ekernel_test_canonical_signatures = .;
        . = ALIGN(64);
    } :data
    .kernel_test_unsafe_signatures : ALIGN(64) {
        __skernel_test_unsafe_signatures = .;
        KEEP(*(.kernel_test_unsafe_signatures))
        __ekernel_test_unsafe_signatures = .;
        . = ALIGN(64);
    } :data

    .kernel_pci_devices : ALIGN(64) {
        __skernel_pci_devices = .;
        KEEP(*(.kernel_pci_devices))
        __ekernel_pci_devices = .;
        . = ALIGN(64);
    } :data
    .kernel_usb_drivers : ALIGN(64) {
        __skernel_usb_drivers = .;
        KEEP(*(.kernel_usb_drivers))
        __ekernel_usb_drivers = .;
        . = ALIGN(64);
    } :data

    .kernel_profiling_data : ALIGN(64) {
        __skernel_profiling_data = .;
        KEEP(*(.kernel_profiling_data))
        __ekernel_profiling_data = .;
        . = ALIGN(64);
    } :data

    .kernel_movealloc_callbacks : ALIGN(64) {
        __skernel_movealloc_callbacks = .;
        KEEP(*(.kernel_movealloc_callbacks))
        __ekernel_movealloc_callbacks = .;
        . = ALIGN(64);
    } :data
    .kernel_slab_sizes : ALIGN(64) {
        __skernel_slab_sizes = .;
        KEEP(*(.kernel_slab_sizes))
        __ekernel_slab_sizes = .;
        . = ALIGN(64);
    } :data
    .kernel_address_ranges : ALIGN(64) {
        __skernel_address_ranges = .;
        KEEP(*(.kernel_address_ranges))
        __ekernel_address_ranges = .;
        . = ALIGN(64);
    } :data

    .kernel_percpu_desc : ALIGN(64) {
        __skernel_percpu_desc = .;
        KEEP(*(.kernel_percpu_desc))
        __ekernel_percpu_desc = .;
        . = ALIGN(64);
    } :data

    .kernel_perdomain_desc : ALIGN(64) {
        __skernel_perdomain_desc = .;
        KEEP(*(.kernel_perdomain_desc))
        __ekernel_perdomain_desc = .;
        . = ALIGN(64);
    } :data
    
    .kernel_pernode_desc : ALIGN(64) {
        __skernel_pernode_desc = .;
        KEEP(*(.kernel_pernode_desc))
        __ekernel_pernode_desc = .;
        . = ALIGN(64);
    } :data

    .kernel_sched_periodic_work : ALIGN(64) {
        __skernel_sched_periodic_work = .;
        KEEP(*(.kernel_sched_periodic_work))
        __ekernel_sched_periodic_work = .;
        . = ALIGN(64);
    } :data

    .kernel_log_sites : ALIGN(64) {
        __skernel_log_sites = .;
        KEEP(*(.kernel_log_sites))
        __ekernel_log_sites = .;
        . = ALIGN(64);
    } :data

    .kernel_cmdline_entries : ALIGN(64) {
        __skernel_cmdline_entries = .;
        KEEP(*(.kernel_cmdline_entries))
        __ekernel_cmdline_entries = .;
        . = ALIGN(64);
    } :data

    .kernel_exception_sync_cbs : ALIGN(64) {
        __skernel_exception_sync_cbs = .;
        KEEP(*(.kernel_exception_sync_cbs))
        __ekernel_exception_sync_cbs = .;
        . = ALIGN(64);
    } :data

    .kernel_bootstage_condition_entries : ALIGN(64) {
        __skernel_bootstage_condition_entries = .;
        KEEP(*(.kernel_bootstage_condition_entries))
        __ekernel_bootstage_condition_entries = .;
        . = ALIGN(64);
    } :data
    
    .kernel_err_facilities : ALIGN(64) {
        __skernel_err_facilities = .;
        KEEP(*(.kernel_err_facilities))
        __ekernel_err_facilities = .;
        . = ALIGN(64);
    } :data

    .kernel_syms : ALIGN(4K) {
        __skernel_syms = .;
        KEEP(*(.kernel_syms))
        . = ALIGN(4K);
        __ekernel_syms = .;
    } :data

    /* BSS */
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
