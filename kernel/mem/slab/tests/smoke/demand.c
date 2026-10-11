#include "mem/slab/tests/test_internal.h"

TEST_DEFINE_SMOKE(slab, demand_growth) {
    size_t count = 500;
    void *ptrs[count];

    for (size_t i = 0; i < count; i++) {
        ptrs[i] = valloc(512);
        TEST_ASSERT_NONNULL(ptrs[i]);
    }

    for (size_t i = 0; i < count; i++) {
        vfree(ptrs[i]);
    }

    return TEST_SUCCESS;
}
