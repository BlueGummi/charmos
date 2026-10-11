#include "mem/tests/test_internal.h"
#include <math/units.h>

TEST_DEFINE_UNIT(mem, valloc_zero_large_reuse) {
    const size_t size = KIB(64);
    for (uint32_t iteration = 0; iteration < 32; iteration++) {
        uint8_t *buffer = valloc(size, ALLOC_ZERO,
                                  .behavior = ALLOC_BEHAVIOR_NORMAL |
                                              ALLOC_BEHAVIOR_FLAG_MINIMAL);
        TEST_ASSERT_NONNULL(buffer);
        bool zero = true;
        for (size_t i = 0; i < size; i++)
            if (buffer[i] != 0) {
                zero = false;
                break;
            }

        /* Dirty the allocation before returning it */
        memset(buffer, 0xa5, size);
        vfree(buffer);
        TEST_ASSERT_MSG(zero, "nonzero allocation on iteration %u", iteration);
    }
    return TEST_SUCCESS;
}
