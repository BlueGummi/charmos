#include "requests.h"
#include <acpi/hpet.h>
#include <acpi/lapic.h>
#include <acpi/print.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/registry.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <drivers/nvme.h>
#include <elf.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/fat.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/cmdline.h>
#include <misc/dbg.h>
#include <misc/linker_symbols.h>
#include <misc/logo.h>
#include <mp/core.h>
#include <mp/mp.h>
#include <pci/pci.h>
#include <pit.h>
#include <rust.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <spin_lock.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <syscall.h>
#include <tests.h>
#include <time/print.h>
#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

void tests_run(void) {
    struct kernel_test *start = &__skernel_tests;
    struct kernel_test *end   = &__ekernel_tests;

    uint64_t test_count = end - start;
    k_info("TEST", K_INFO, "Running %llu tests...", test_count);

    for (struct kernel_test *t = start; t < end; t++) {
        k_info("TEST", K_INFO, "Running %s", t->name);
        t->func();
        k_info("TEST", K_INFO, "PASS %s", t->name);
    }

    k_info("TEST", K_INFO, "All tests passed");
}


REGISTER_TEST(pmm_alloc_test) {
    void *p = pmm_alloc_page(false);
    assert(p != NULL);
    pmm_free_pages(p, 1, false);
}

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    assert(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    REGISTER_TEST(kmalloc_aligned_##name##_test) {                             \
        void *ptr = kmalloc_aligned(align, align);                             \
        assert(ptr != NULL);                                                   \
        ASSERT_ALIGNED(ptr, align);                                            \
        kfree_aligned(ptr);                                                    \
    }

KMALLOC_ALIGNMENT_TEST(8, 8)
KMALLOC_ALIGNMENT_TEST(16, 16)
KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)
KMALLOC_ALIGNMENT_TEST(4096, 4096)
