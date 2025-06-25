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

struct kernel_test *current_test = NULL;
void tests_run(void) {
    struct kernel_test *start = __skernel_tests;
    struct kernel_test *end = __ekernel_tests;

    uint64_t test_count =
        ((uint64_t) end - (uint64_t) start) / sizeof(struct kernel_test);
    k_info("TEST", K_TEST, "running %llu tests...\n", test_count);

    uint64_t pass_count = 0, skip_count = 0, fail_count = 0, i = 1;
    for (struct kernel_test *t = start; t < end; t++, i++) {
        current_test = t;
        k_printf("[%-4d]: ", i);
        k_printf("%s... ", t->name);

        uint64_t start_ms = hpet_timestamp_ms();
        /* supa important */
        t->func();
        uint64_t end_ms = hpet_timestamp_ms();

        if (t->skipped) {
            k_printf(ANSI_GRAY " skipped  " ANSI_RESET);
            skip_count++;
        } else if (t->success != t->should_fail) {
            k_printf(ANSI_GREEN " ok  " ANSI_RESET);
            pass_count++;
        } else {
            k_printf(ANSI_RED " error  " ANSI_RESET);
            fail_count++;
        }

        k_printf("(%llu ms)\n", end_ms - start_ms);

        if (t->message_count > 0) {
            for (uint64_t i = 0; i < t->message_count; i++) {
                k_printf("        +-> ");
                k_printf(ANSI_YELLOW "%s" ANSI_RESET "\n", t->messages[i]);
            }
            k_printf("\n");
        }
    }

    bool all_ok = fail_count == 0;
    char *color = all_ok ? ANSI_GREEN : ANSI_RED;
    char *msg = all_ok ? "all ok!\n" : "some errors occurred\n";
    char *fail_color = all_ok ? ANSI_GREEN : ANSI_RED;
    char *skip_color = all_ok ? ANSI_GREEN : ANSI_GRAY;

    k_info("TEST", K_TEST,
           "%llu " ANSI_GREEN "passed" ANSI_RESET ", %llu %sfailed" ANSI_RESET
           ", %llu %sskipped\n" ANSI_RESET,
           pass_count, fail_count, fail_color, skip_count, skip_color);

    k_info("TEST", K_TEST, "%s%s" ANSI_RESET, color, msg);
}

REGISTER_TEST(pmm_alloc_test, false) {
    void *p = pmm_alloc_page(false);
    TEST_ASSERT(p != NULL);
    pmm_free_pages(p, 1, false);
    SET_SUCCESS;
}

REGISTER_TEST(ext2_withdisk_test, false) {
    if (g_root_node->fs_type != FS_EXT2) {
        ADD_MESSAGE("the mounted root is not ext2");
        SET_SKIP;
        return;
    }
    struct vfs_node *root = g_root_node;

    enum errno e = root->ops->create(root, "banana", VFS_MODE_FILE);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *node = root->ops->finddir(root, "banana");
    TEST_ASSERT(node != NULL);

    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    e = node->ops->write(node, lstr, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->size == len);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(memcmp(out_buf, lstr, len) == 0);

    e = node->ops->truncate(node, len / 2);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    memset(out_buf, 0, len);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(strlen(out_buf) == len / 2);

    e = node->ops->unlink(root, "banana");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = root->ops->finddir(root, "banana");
    TEST_ASSERT(node == NULL);

    SET_SUCCESS;
}

REGISTER_TEST(vmm_map_test, false) {
    uint64_t p = (uint64_t) pmm_alloc_page(false);
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_phys(p, 4096);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, 4096);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr) == (uint64_t) -1);
    SET_SUCCESS;
}

#define TMPFS_SETUP_NODE(root, node, name, e)                                  \
    struct vfs_node *root = tmpfs_mkroot("tmp");                               \
    TEST_ASSERT(root != NULL);                                                 \
    enum errno e = root->ops->create(root, name, VFS_MODE_FILE);               \
    struct vfs_node *node = root->ops->finddir(root, name);                    \
    TEST_ASSERT(node != NULL);

REGISTER_TEST(tmpfs_rw_test, false) {
    TMPFS_SETUP_NODE(root, node, "place", e);
    TEST_ASSERT(node->size == 0);

    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    e = node->ops->write(node, lstr, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->size == len);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(memcmp(out_buf, lstr, len) == 0);

    e = node->ops->truncate(node, len / 2);
    TEST_ASSERT(!ERR_IS_FATAL(e));
    TEST_ASSERT(node->size == len / 2);

    memset(out_buf, 0, len);
    e = node->ops->read(node, out_buf, len, 0);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = node->ops->unlink(root, "place");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    e = node->ops->destroy(node);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = root->ops->finddir(root, "place");
    TEST_ASSERT(node == NULL);

    TEST_ASSERT(strlen(out_buf) == len / 2);
    SET_SUCCESS;
}

REGISTER_TEST(tmpfs_dir_test, false) {
    struct vfs_node *root = tmpfs_mkroot("tmp");
    const char *lstr = large_test_string;
    uint64_t len = strlen(lstr);

    char *out_buf = kzalloc(len);
    TEST_ASSERT(out_buf != NULL);

    enum errno e = root->ops->mkdir(root, "place", VFS_MODE_DIR);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *dir = root->ops->finddir(root, "place");
    TEST_ASSERT(dir != NULL);

    e = dir->ops->write(dir, lstr, len, 0);
    TEST_ASSERT(e == ERR_IS_DIR);

    e = dir->ops->read(dir, out_buf, len, 0);
    TEST_ASSERT(e == ERR_IS_DIR);

    e = dir->ops->rmdir(root, "place");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    dir = root->ops->finddir(root, "place");
    TEST_ASSERT(dir == NULL);

    SET_SUCCESS;
}

REGISTER_TEST(tmpfs_general_tests, false) {
    TMPFS_SETUP_NODE(root, node, "place", e);

    e = node->ops->chmod(node, VFS_MODE_EXEC);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(node->mode == VFS_MODE_EXEC);

    e = node->ops->chown(node, 42, 37);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(node->uid == 42 && node->gid == 37);

    e = root->ops->mkdir(root, "bingbong", VFS_MODE_DIR);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    node = root->ops->finddir(root, "bingbong");

    e = node->ops->symlink(node, "/tmp", "bang");
    TEST_ASSERT(!ERR_IS_FATAL(e));

    struct vfs_node *bang = node->ops->finddir(node, "bang");
    TEST_ASSERT(bang != NULL);

    char *buf = kzalloc(10);
    TEST_ASSERT(bang != NULL);

    e = bang->ops->readlink(bang, buf, 10);
    TEST_ASSERT(!ERR_IS_FATAL(e));

    TEST_ASSERT(strcmp(buf, "/tmp") == 0);

    SET_SUCCESS;
}

/* probably don't need these at all but I'll keep
 * them in case something decides to be funny */
#define ALIGNED_ALLOC_TIMES 512

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    REGISTER_TEST(kmalloc_aligned_##name##_test, false) {                      \
        for (uint64_t i = 0; i < ALIGNED_ALLOC_TIMES; i++) {                   \
            void *ptr = kmalloc_aligned(align, align);                         \
            TEST_ASSERT(ptr != NULL);                                          \
            ASSERT_ALIGNED(ptr, align);                                        \
            kfree_aligned(ptr);                                                \
        }                                                                      \
        SET_SUCCESS;                                                           \
    }

KMALLOC_ALIGNMENT_TEST(8, 8)
KMALLOC_ALIGNMENT_TEST(16, 16)
KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)
KMALLOC_ALIGNMENT_TEST(4096, 4096)
