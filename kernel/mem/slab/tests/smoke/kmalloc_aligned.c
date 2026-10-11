#include "mem/slab/tests/test_internal.h"

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT_EQ(((uintptr_t) (ptr) & ((alignment) - 1)), 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    TEST_DEFINE_SMOKE(slab, valloc_aligned_##name,                            \
                      TEST_INTENSITY(32, 512, 2048), .min_ram_mib = 8) {       \
        size_t alloc_times = ctx->intensity_val ? ctx->intensity_val : 512;    \
        for (uint64_t i = 0; i < alloc_times; i++) {                           \
            void *ptr = valloc_aligned(align, align);                         \
            TEST_ASSERT_NONNULL(ptr);                                          \
            ASSERT_ALIGNED(ptr, align);                                        \
            vfree_aligned(ptr);                                                \
        }                                                                      \
        return TEST_SUCCESS;                                                   \
    }

KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)
