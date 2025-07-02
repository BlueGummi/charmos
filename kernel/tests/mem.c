#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>

REGISTER_TEST(pmm_alloc_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    void *p = pmm_alloc_page(false);
    TEST_ASSERT(p != NULL);
    pmm_free_pages(p, 1, false);
    SET_SUCCESS;
}

REGISTER_TEST(vmm_map_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint64_t p = (uint64_t) pmm_alloc_page(false);
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_phys(p, PAGE_SIZE);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, PAGE_SIZE);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr) == (uint64_t) -1);
    SET_SUCCESS;
}

/* probably don't need these at all but I'll keep
 * them in case something decides to be funny */
#define ALIGNED_ALLOC_TIMES 512

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    REGISTER_TEST(kmalloc_aligned_##name##_test, false, false) {               \
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
KMALLOC_ALIGNMENT_TEST(PAGE_SIZE, PAGE_SIZE)
