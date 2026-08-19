#include "../test_internal.h"

#ifdef TEST_MEM


#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    TEST_DECLARE_SMOKE(kmalloc_aligned_##name##_test,                          \
                       .group = TEST_GROUP(slab),                              \
                       TEST_INTENSITY(32, 512, 2048)) {                        \
        ABORT_IF_RAM_LOW();                                                    \
        size_t alloc_times = ctx->intensity_val ? ctx->intensity_val : 512;    \
        for (uint64_t i = 0; i < alloc_times; i++) {                           \
            void *ptr = kmalloc_aligned(align, align);                         \
            TEST_ASSERT(ptr != NULL);                                          \
            ASSERT_ALIGNED(ptr, align);                                        \
            kfree_aligned(ptr);                                                \
        }                                                                      \
        return TEST_SUCCESS;                                                   \
    }

KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)
#endif
