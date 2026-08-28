#include "mem/slab/tests/test_internal.h"

#ifdef TEST_MM

TEST_DECLARE_SMOKE(slab, demand_growth) {
    size_t count = 500;
    void *ptrs[count];

    for (size_t i = 0; i < count; i++) {
        ptrs[i] = kmalloc(512);
        TEST_ASSERT_NONNULL(ptrs[i]);
    }

    for (size_t i = 0; i < count; i++) {
        kfree(ptrs[i]);
    }

    return TEST_SUCCESS;
}
#endif
