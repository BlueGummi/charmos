#include "../test_internal.h"

#ifdef TEST_BIT_OPS
TEST_GROUP_DECLARE(bit_ops, .intensity_desc = {
                                .curve = TEST_SCALE_PIECEWISE_LOG,
                                .unit = "iters",
                            });

TEST_DECLARE_UNIT(next_pow2_standard, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(next_pow2(1) == 1);
    TEST_ASSERT(next_pow2(2) == 2);
    TEST_ASSERT(next_pow2(3) == 4);
    TEST_ASSERT(next_pow2(4) == 4);
    TEST_ASSERT(next_pow2(5) == 8);
    TEST_ASSERT(next_pow2(4096) == 4096);
    TEST_ASSERT(next_pow2(4097) == 8192);

    /* Exact powers must be fixed points, and one past must step once,
     * p - 1 only rounds up to p from shift 2 and on */
    for (size_t shift = 0; shift < 63; shift++) {
        size_t p = (size_t) 1 << shift;
        TEST_ASSERT(next_pow2(p) == p);
        if (shift >= 2)
            TEST_ASSERT(next_pow2(p - 1) == p);
        TEST_ASSERT(next_pow2(p + 1) == p << 1);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(next_pow2_edges, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(next_pow2(0) == 1);

    /* Must avoid infinite loop */
    size_t top = (size_t) 1 << 63;
    TEST_ASSERT(next_pow2(top) == top);
    TEST_ASSERT(next_pow2(top + 1) == top);
    TEST_ASSERT(next_pow2(SIZE_MAX) == top);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(prev_pow2_standard, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(prev_pow2(1) == 1);
    TEST_ASSERT(prev_pow2(2) == 2);
    TEST_ASSERT(prev_pow2(3) == 2);
    TEST_ASSERT(prev_pow2(4) == 4);
    TEST_ASSERT(prev_pow2(5) == 4);
    TEST_ASSERT(prev_pow2(4096) == 4096);
    TEST_ASSERT(prev_pow2(4095) == 2048);

    for (size_t shift = 1; shift < 63; shift++) {
        size_t p = (size_t) 1 << shift;
        TEST_ASSERT(prev_pow2(p) == p);
        TEST_ASSERT(prev_pow2(p - 1) == p >> 1);
        TEST_ASSERT(prev_pow2(p + 1) == p);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(prev_pow2_edges, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(prev_pow2(0) == 1);

    size_t top = (size_t) 1 << 63;
    TEST_ASSERT(prev_pow2(top) == top);
    TEST_ASSERT(prev_pow2(SIZE_MAX) == top);

    return TEST_SUCCESS;
}

/* prev_pow2(x) <= x <= next_pow2(x) must always be true */
TEST_DECLARE_UNIT(pow2_bracket_invariant, .group = TEST_GROUP(bit_ops),
                  TEST_INTENSITY(256, 4096, 65536)) {
    size_t iters = ctx->intensity_val ? ctx->intensity_val : 4096;
    for (size_t x = 1; x <= iters; x++) {
        size_t lo = prev_pow2(x);
        size_t hi = next_pow2(x);

        TEST_ASSERT(lo <= x && x <= hi);
        TEST_ASSERT((lo & (lo - 1)) == 0);
        TEST_ASSERT((hi & (hi - 1)) == 0);
        TEST_ASSERT(hi <= lo * 2);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ilog2_standard, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(ilog2(1) == 0);
    TEST_ASSERT(ilog2(2) == 1);
    TEST_ASSERT(ilog2(3) == 1);
    TEST_ASSERT(ilog2(4) == 2);
    TEST_ASSERT(ilog2(4096) == 12);

    /* p + 1 keeps same floor except at p == 1, where next int
     * is itself the next pow2 */
    for (uint8_t shift = 0; shift < 64; shift++) {
        uint64_t p = (uint64_t) 1 << shift;
        TEST_ASSERT(ilog2(p) == shift);
        if (shift > 0)
            TEST_ASSERT(ilog2(p - 1) == shift - 1);
        if (shift >= 1 && shift < 63)
            TEST_ASSERT(ilog2(p + 1) == shift);
    }

    return TEST_SUCCESS;
}

/* ilog2(0) == 0, which is also ilog2(1) */
TEST_DECLARE_UNIT(ilog2_edges, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(ilog2(0) == 0);
    TEST_ASSERT(ilog2(UINT64_MAX) == 63);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(popcount_standard, .group = TEST_GROUP(bit_ops)) {
    TEST_ASSERT(popcount(0) == 0);
    TEST_ASSERT(popcount(1) == 1);
    TEST_ASSERT(popcount(3) == 2);
    TEST_ASSERT(popcount(SIZE_MAX) == 64);
    TEST_ASSERT(popcount(0x5555555555555555ULL) == 32);
    TEST_ASSERT(popcount(0xAAAAAAAAAAAAAAAAULL) == 32);

    for (size_t shift = 0; shift < 64; shift++)
        TEST_ASSERT(popcount((size_t) 1 << shift) == 1);

    return TEST_SUCCESS;
}

#endif
