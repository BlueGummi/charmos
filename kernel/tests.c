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
#include <fs/tmpfs.h>
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

extern struct kernel_test __skernel_tests[];
extern struct kernel_test __ekernel_tests[];

/* no need to clean up allocations in these tests, we are supposed to
 * reboot/poweroff after all tests complete, and the userland should
 * not be in a state where we can boot it when running tests */

void tests_run(void) {
    struct kernel_test *start = __skernel_tests;
    struct kernel_test *end = __ekernel_tests;

    uint64_t test_count =
        ((uint64_t) end - (uint64_t) start) / sizeof(struct kernel_test);
    k_info("TEST", K_TEST, ANSI_CYAN "running" ANSI_RESET " %llu tests...\n",
           test_count);

    for (struct kernel_test *t = start; t < end; t++) {
        k_info("TEST", K_TEST, ANSI_CYAN "running" ANSI_RESET " %s... ",
               t->name);
        t->func();
        if ((!t->success && t->should_fail) ||
            (t->success && !t->should_fail)) {
            k_printf(ANSI_GREEN " ok\n" ANSI_RESET);
        } else {
            k_printf(ANSI_RED " error\n" ANSI_RESET);
            asm("cli;hlt");
        }
    }

    k_info("TEST", K_TEST, ANSI_GREEN "all ok!\n" ANSI_RESET);
}

REGISTER_TEST(pmm_alloc_test, false) {
    void *p = pmm_alloc_page(false);
    test_assert(p != NULL);
    pmm_free_pages(p, 1, false);
    SET_SUCCESS(pmm_alloc_test);
}

REGISTER_TEST(vmm_map_test, false) {
    uint64_t p = (uint64_t) pmm_alloc_page(false);
    test_assert(p != 0);
    void *ptr = vmm_map_phys(p, 4096);
    test_assert(ptr != NULL);
    vmm_unmap_virt(ptr, 4096);
    test_assert(vmm_get_phys((uint64_t) ptr) == (uint64_t) -1);
    SET_SUCCESS(vmm_map_test);
}

REGISTER_TEST(tmpfs_rw_test, false) {
    struct vfs_node *root = tmpfs_mkroot("tmp");
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    char *out_buf = kzalloc(len);
    test_assert(out_buf != NULL);

    enum errno e = root->ops->create(root, "place", VFS_MODE_FILE);
    test_assert(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "place");
    test_assert(node != NULL);
    test_assert(node->size == 0);

    e = node->ops->write(node, lstr, len, 0);
    test_assert(!ERR_IS_FATAL(e));
    test_assert(node->size == len);

    e = node->ops->read(node, out_buf, len, 0);
    test_assert(!ERR_IS_FATAL(e));

    test_assert(memcmp(out_buf, lstr, len) == 0);

    e = node->ops->truncate(node, len / 2);
    test_assert(!ERR_IS_FATAL(e));
    test_assert(node->size = len / 2);

    memset(out_buf, 0, len);
    e = node->ops->read(node, out_buf, len, 0);
    test_assert(!ERR_IS_FATAL(e));

    test_assert(strlen(out_buf) == len / 2);

    SET_SUCCESS(tmpfs_rw_test);
}

REGISTER_TEST(tmpfs_dir_test, false) {
    struct vfs_node *root = tmpfs_mkroot("tmp");
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    char *out_buf = kzalloc(len);
    test_assert(out_buf != NULL);

    enum errno e = root->ops->mkdir(root, "place", VFS_MODE_DIR);
    test_assert(!ERR_IS_FATAL(e));

    struct vfs_node *dir = root->ops->finddir(root, "place");
    test_assert(dir != NULL);

    SET_SUCCESS(tmpfs_dir_test);
}

#define ALIGNED_ALLOC_TIMES 512

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    test_assert(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    REGISTER_TEST(kmalloc_aligned_##name##_test, false) {                      \
        for (uint64_t i = 0; i < ALIGNED_ALLOC_TIMES; i++) {                   \
            void *ptr = kmalloc_aligned(align, align);                         \
            test_assert(ptr != NULL);                                          \
            ASSERT_ALIGNED(ptr, align);                                        \
            kfree_aligned(ptr);                                                \
        }                                                                      \
        SET_SUCCESS(kmalloc_aligned_##name##_test);                            \
    }

KMALLOC_ALIGNMENT_TEST(8, 8)
KMALLOC_ALIGNMENT_TEST(16, 16)
KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)
KMALLOC_ALIGNMENT_TEST(4096, 4096)
