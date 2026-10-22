#include "math/tests/test_internal.h"

TEST_GROUP_DEFINE(bit_ops, .intensity_desc = {
                               .curve = SCALE_PIECEWISE_LOG,
                               .unit = "iters",
                           });

TEST_DEFINE_UNIT(bit_ops, next_pow2_standard) {
    TEST_ASSERT_EQ(next_pow2(1), 1);
    TEST_ASSERT_EQ(next_pow2(2), 2);
    TEST_ASSERT_EQ(next_pow2(3), 4);
    TEST_ASSERT_EQ(next_pow2(4), 4);
    TEST_ASSERT_EQ(next_pow2(5), 8);
    TEST_ASSERT_EQ(next_pow2(4096), 4096);
    TEST_ASSERT_EQ(next_pow2(4097), 8192);

    /* Exact powers must be fixed points, and one past must step once,
     * p - 1 only rounds up to p from shift 2 and on */
    for (size_t shift = 0; shift < 63; shift++) {
        size_t p = (size_t) 1 << shift;
        TEST_ASSERT_EQ(next_pow2(p), p);
        if (shift >= 2)
            TEST_ASSERT_EQ(next_pow2(p - 1), p);
        TEST_ASSERT_EQ(next_pow2(p + 1), p << 1);
    }

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, next_pow2_edge) {
    TEST_ASSERT_EQ(next_pow2(0), 1);

    /* Must avoid infinite loop */
    size_t top = (size_t) 1 << 63;
    TEST_ASSERT_EQ(next_pow2(top), top);
    TEST_ASSERT_EQ(next_pow2(top + 1), top);
    TEST_ASSERT_EQ(next_pow2(SIZE_MAX), top);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, prev_pow2_standard) {
    TEST_ASSERT_EQ(prev_pow2(1), 1);
    TEST_ASSERT_EQ(prev_pow2(2), 2);
    TEST_ASSERT_EQ(prev_pow2(3), 2);
    TEST_ASSERT_EQ(prev_pow2(4), 4);
    TEST_ASSERT_EQ(prev_pow2(5), 4);
    TEST_ASSERT_EQ(prev_pow2(4096), 4096);
    TEST_ASSERT_EQ(prev_pow2(4095), 2048);

    for (size_t shift = 1; shift < 63; shift++) {
        size_t p = (size_t) 1 << shift;
        TEST_ASSERT_EQ(prev_pow2(p), p);
        TEST_ASSERT_EQ(prev_pow2(p - 1), p >> 1);
        TEST_ASSERT_EQ(prev_pow2(p + 1), p);
    }

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, prev_pow2_edge) {
    TEST_ASSERT_EQ(prev_pow2(0), 1);

    size_t top = (size_t) 1 << 63;
    TEST_ASSERT_EQ(prev_pow2(top), top);
    TEST_ASSERT_EQ(prev_pow2(SIZE_MAX), top);

    return TEST_SUCCESS;
}

/* prev_pow2(x) <= x <= next_pow2(x) must always be true */
TEST_DEFINE_UNIT(bit_ops, pow2_bracket_invariant,
                 TEST_INTENSITY(256, 4096, 65536)) {
    size_t iters = ctx->intensity_val ? ctx->intensity_val : 4096;
    for (size_t x = 1; x <= iters; x++) {
        size_t lo = prev_pow2(x);
        size_t hi = next_pow2(x);

        TEST_ASSERT_IN_RANGE(x, lo, hi);
        TEST_ASSERT_EQ((lo & (lo - 1)), 0);
        TEST_ASSERT_EQ((hi & (hi - 1)), 0);
        TEST_ASSERT_LE(hi, lo * 2);
    }

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, ilog2_standard) {
    TEST_ASSERT_EQ(ilog2(1), 0);
    TEST_ASSERT_EQ(ilog2(2), 1);
    TEST_ASSERT_EQ(ilog2(3), 1);
    TEST_ASSERT_EQ(ilog2(4), 2);
    TEST_ASSERT_EQ(ilog2(4096), 12);

    /* p + 1 keeps same floor except at p == 1, where next int
     * is itself the next pow2 */
    for (uint8_t shift = 0; shift < 64; shift++) {
        uint64_t p = (uint64_t) 1 << shift;
        TEST_ASSERT_EQ(ilog2(p), shift);
        if (shift > 0)
            TEST_ASSERT_EQ(ilog2(p - 1), shift - 1);
        if (shift >= 1 && shift < 63)
            TEST_ASSERT_EQ(ilog2(p + 1), shift);
    }

    return TEST_SUCCESS;
}

/* ilog2(0) == 0, which is also ilog2(1) */
TEST_DEFINE_UNIT(bit_ops, ilog2_edge) {
    TEST_ASSERT_EQ(ilog2(0), 0);
    TEST_ASSERT_EQ(ilog2(UINT64_MAX), 63);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, popcount_standard) {
    TEST_ASSERT_EQ(popcount(0), 0);
    TEST_ASSERT_EQ(popcount(1), 1);
    TEST_ASSERT_EQ(popcount(3), 2);
    TEST_ASSERT_EQ(popcount(SIZE_MAX), 64);
    TEST_ASSERT_EQ(popcount(0x5555555555555555ULL), 32);
    TEST_ASSERT_EQ(popcount(0xAAAAAAAAAAAAAAAAULL), 32);

    for (size_t shift = 0; shift < 64; shift++)
        TEST_ASSERT_EQ(popcount((size_t) 1 << shift), 1);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, bit_macros_standard) {
    uint8_t u8 = 0;
    u8 = BIT_SET(u8, 2);
    TEST_ASSERT_EQ(u8, 4);
    TEST_ASSERT_EQ(sizeof(BIT_SET(u8, 2)), sizeof(uint8_t));
    TEST_ASSERT(BIT_TEST(u8, 2));
    TEST_ASSERT(!BIT_TEST(u8, 3));

    u8 = BIT_TOGGLE(u8, 2);
    TEST_ASSERT_EQ(u8, 0);
    TEST_ASSERT_EQ(sizeof(BIT_TOGGLE(u8, 2)), sizeof(uint8_t));

    uint32_t u32 = 0x12345678;
    u32 = BIT_CLEAR(u32, 3);
    TEST_ASSERT_EQ(u32, 0x12345670);
    TEST_ASSERT_EQ(sizeof(BIT_CLEAR(u32, 3)), sizeof(uint32_t));

    TEST_ASSERT(BIT_ANY(u32, UINT32_C(0x70)));
    TEST_ASSERT(!BIT_ANY(u32, UINT32_C(0x80)));
    TEST_ASSERT(BIT_ALL(u32, UINT32_C(0x12345670)));
    TEST_ASSERT(!BIT_ALL(u32, UINT32_C(0x12345678)));

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, bit_fields_and_ranges) {
    uint32_t reg32 = 0;
    reg32 = BIT_SET_FIELD(reg32, 0xA, 4, 7);
    TEST_ASSERT_EQ(reg32, 0xA0);
    TEST_ASSERT_EQ(sizeof(BIT_SET_FIELD(reg32, 0xA, 4, 7)), sizeof(uint32_t));
    TEST_ASSERT_EQ(BIT_GET_FIELD(reg32, 4, 7), 0xA);
    TEST_ASSERT_EQ(sizeof(BIT_GET_FIELD(reg32, 4, 7)), sizeof(uint32_t));

    uint64_t reg64 = 0;
    reg64 = BIT_SET_FIELD(reg64, 0xDEADBEEFULL, 32, 63);
    TEST_ASSERT_EQ(BIT_GET_FIELD(reg64, 32, 63), 0xDEADBEEFULL);

    TEST_ASSERT_EQ(BIT_MASK(0, 0), 1ULL);
    TEST_ASSERT_EQ(BIT_MASK(4, 7), 0xF0ULL);
    TEST_ASSERT_EQ(BIT_MASK(0, 63), UINT64_MAX);

    TEST_ASSERT_EQ(BIT_RANGE(0xABCD, 4, 7), 0xC);
    TEST_ASSERT_EQ(BIT_WIDTH(4, 7), 4);
    TEST_ASSERT_EQ(BIT_WIDTH(0, 63), 64);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, align_and_rounding) {
    size_t value = 5;
    size_t alignment = 4;
    TEST_ASSERT_EQ(ALIGN_UP(value++, alignment++), 8);
    TEST_ASSERT_EQ(value, 6);
    TEST_ASSERT_EQ(alignment, 5);

    value = 9;
    alignment = 8;
    TEST_ASSERT_EQ(ALIGN_DOWN(value++, alignment++), 8);
    TEST_ASSERT_EQ(value, 10);
    TEST_ASSERT_EQ(alignment, 9);

    value = 8;
    alignment = 8;
    TEST_ASSERT(IS_ALIGNED(value++, alignment++));
    TEST_ASSERT_EQ(value, 9);
    TEST_ASSERT_EQ(alignment, 9);

    value = 4;
    alignment = 3;
    TEST_ASSERT(!IS_ALIGNED_UNCHECKED(value++, alignment++));
    TEST_ASSERT_EQ(value, 5);
    TEST_ASSERT_EQ(alignment, 4);
    alignment = 0;
    TEST_ASSERT(!IS_ALIGNED_UNCHECKED(value, alignment));
    TEST_ASSERT_EQ(ALIGN_UP_WRAPPING(SIZE_MAX, 2), 0);

    size_t divisor = 4;
    value = 5;
    TEST_ASSERT_EQ(DIV_ROUND_UP(value++, divisor++), 2);
    TEST_ASSERT_EQ(value, 6);
    TEST_ASSERT_EQ(divisor, 5);
    TEST_ASSERT_EQ(DIV_ROUND_UP(SIZE_MAX, 2), SIZE_MAX / 2 + 1);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, abs_and_pow2_macros) {
    int value = -3;
    TEST_ASSERT_EQ(abs(value++), 3);
    TEST_ASSERT_EQ(value, -2);
    value = 0;
    TEST_ASSERT_EQ(abs(value), 0);
    value = 3;
    TEST_ASSERT_EQ(abs(value), 3);

    size_t alignment = 8;
    TEST_ASSERT(IS_POW2(alignment++));
    TEST_ASSERT_EQ(alignment, 9);
    TEST_ASSERT(!IS_POW2(alignment));
    TEST_ASSERT(IS_POW2(SIZE_MAX / 2 + 1));
    TEST_ASSERT(!IS_POW2((size_t) 0));

    int signed_align = -8;
    TEST_ASSERT(!IS_POW2(signed_align));

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, common_domain_comparisons) {
    uint32_t small = 5;
    uint64_t big = 9;
    TEST_ASSERT_EQ(MIN(small, big), 5);
    TEST_ASSERT_EQ(MIN(big, small), 5);
    TEST_ASSERT_EQ(MAX(small, big), 9);
    TEST_ASSERT_EQ(MAX(big, small), 9);
    TEST_ASSERT_EQ(sizeof(MIN(small, big)), sizeof(uint64_t));
    TEST_ASSERT_EQ(sizeof(MIN(big, small)), sizeof(uint64_t));

    int64_t wide_signed = -4;
    TEST_ASSERT_EQ(MIN(small, wide_signed), -4);
    TEST_ASSERT_EQ(sizeof(MIN(small, wide_signed)), sizeof(int64_t));

    int32_t narrow_signed = -3;
    TEST_ASSERT_EQ(MAX(narrow_signed, wide_signed), -3);

    size_t len = 7;
    TEST_ASSERT_EQ(MIN(len, 0), 0);
    TEST_ASSERT_EQ(MIN(0, len), 0);
    TEST_ASSERT_EQ(MAX(len, 1), 7);
    TEST_ASSERT_EQ(sizeof(MIN(len, 0)), sizeof(size_t));

    uint8_t a8 = 200;
    uint8_t b8 = 100;
    TEST_ASSERT_EQ(MIN(a8, b8), 100);
    TEST_ASSERT_EQ(sizeof(MIN(a8, b8)), sizeof(uint8_t));
    TEST_ASSERT_EQ(sizeof(MAX(a8, b8)), sizeof(uint8_t));

    uint32_t left = 4;
    uint32_t right = 6;
    TEST_ASSERT_EQ(MIN(left++, right++), 4);
    TEST_ASSERT_EQ(left, 5);
    TEST_ASSERT_EQ(right, 7);

    TEST_ASSERT_EQ(MAX(small, big, (uint64_t) 2), 9);
    TEST_ASSERT_EQ(MIN(small, big, (uint64_t) 2), 2);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, common_domain_range_and_clamp) {
    uint64_t value = 40;
    uint32_t low = 10;
    TEST_ASSERT(IN_RANGE(value, low, 64));
    TEST_ASSERT(!IN_RANGE(value, low, 32));
    TEST_ASSERT(IN_RANGE(value, 0, value));

    int32_t signed_value = -5;
    TEST_ASSERT(IN_RANGE(signed_value, -19, 20));
    TEST_ASSERT(!IN_RANGE(signed_value, 0, 20));

    uint32_t narrow = 500;
    uint64_t ceiling = 100;
    narrow = CLAMP(narrow, 0, ceiling);
    TEST_ASSERT_EQ(narrow, 100);
    TEST_ASSERT_EQ(sizeof(narrow), sizeof(uint32_t));

    int32_t delta = -80;
    delta = CLAMP(delta, -20, 20);
    TEST_ASSERT_EQ(delta, -20);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, bit_macros_preserve_operand_type) {
    uint32_t reg = 0x80000000u;
    TEST_ASSERT_EQ(sizeof(BIT_TEST(reg, 31)), sizeof(bool));
    TEST_ASSERT(BIT_TEST(reg, 31));
    TEST_ASSERT(!BIT_TEST(reg, 30));

    uint32_t index = 31;
    TEST_ASSERT(BIT_TEST(reg, index++));
    TEST_ASSERT_EQ(index, 32);

    uint32_t packed = 0x0000ABCDu;
    TEST_ASSERT_EQ(BIT_RANGE(packed, 4, 7), 0xC);
    TEST_ASSERT_EQ(sizeof(BIT_RANGE(packed, 4, 7)), sizeof(uint32_t));

    uint64_t wide = UINT64_C(0xDEADBEEF) << 32;
    TEST_ASSERT_EQ(BIT_RANGE(wide, 32, 63), UINT64_C(0xDEADBEEF));
    TEST_ASSERT_EQ(sizeof(BIT_RANGE(wide, 32, 63)), sizeof(uint64_t));

    TEST_ASSERT(BIT_ANY(packed, 0xF0));
    TEST_ASSERT(!BIT_ANY(packed, 0xF0000000u));
    TEST_ASSERT(BIT_ALL(packed, 0xABCD));
    TEST_ASSERT_EQ(sizeof(BIT_ANY(packed, 0xF0)), sizeof(bool));

    uint32_t lo = 4;
    TEST_ASSERT_EQ(BIT_MASK(lo++, 7), 0xF0ULL);
    TEST_ASSERT_EQ(lo, 5);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, compiler_wrappers_popcount_and_bswap) {
    uint8_t u8 = 0b1011;
    uint16_t u16 = 0x00FF;
    uint32_t u32 = 0x00FF00FF;
    uint64_t u64 = 0x5555555555555555ULL;

    TEST_ASSERT_EQ(cw_popcount(u8), 3);
    TEST_ASSERT_EQ(cw_popcount(u16), 8);
    TEST_ASSERT_EQ(cw_popcount(u32), 16);
    TEST_ASSERT_EQ(cw_popcount(u64), 32);

    TEST_ASSERT_EQ(cw_bswap((uint16_t) 0x1234), 0x3412);
    TEST_ASSERT_EQ(cw_bswap((uint32_t) 0x12345678), 0x78563412);
    TEST_ASSERT_EQ(cw_bswap((uint64_t) 0x0102030405060708ULL),
                   0x0807060504030201ULL);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, compiler_wrappers_clz_ctz_ffs_fls) {
    TEST_ASSERT_EQ(cw_clz((uint8_t) 0), 8);
    TEST_ASSERT_EQ(cw_clz((uint8_t) 1), 7);
    TEST_ASSERT_EQ(cw_clz((uint8_t) 0x80), 0);

    TEST_ASSERT_EQ(cw_clz((uint32_t) 0), 32);
    TEST_ASSERT_EQ(cw_clz((uint32_t) 1), 31);
    TEST_ASSERT_EQ(cw_clz((uint32_t) 0x80000000), 0);

    TEST_ASSERT_EQ(cw_clz((uint64_t) 0), 64);
    TEST_ASSERT_EQ(cw_clz((uint64_t) 1), 63);

    TEST_ASSERT_EQ(cw_ctz((uint8_t) 0), 8);
    TEST_ASSERT_EQ(cw_ctz((uint8_t) 2), 1);
    TEST_ASSERT_EQ(cw_ctz((uint32_t) 0), 32);
    TEST_ASSERT_EQ(cw_ctz((uint32_t) 8), 3);
    TEST_ASSERT_EQ(cw_ctz((uint64_t) 0), 64);
    TEST_ASSERT_EQ(cw_ctz((uint64_t) 16), 4);

    TEST_ASSERT_EQ(cw_ffs((uint32_t) 0), 0);
    TEST_ASSERT_EQ(cw_ffs((uint32_t) 1), 1);
    TEST_ASSERT_EQ(cw_ffs((uint32_t) 0x10), 5);

    TEST_ASSERT_EQ(cw_fls((uint32_t) 0), 0);
    TEST_ASSERT_EQ(cw_fls((uint32_t) 1), 1);
    TEST_ASSERT_EQ(cw_fls((uint32_t) 0x10), 5);
    TEST_ASSERT_EQ(cw_fls((uint32_t) 0x80000000), 32);

    TEST_ASSERT_EQ(cw_parity((uint32_t) 0), 0);
    TEST_ASSERT_EQ(cw_parity((uint32_t) 1), 1);
    TEST_ASSERT_EQ(cw_parity((uint32_t) 3), 0);
    TEST_ASSERT_EQ(cw_parity((uint32_t) 7), 1);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(bit_ops, compiler_wrappers_rotations_and_saturation) {
    uint32_t val32 = 0x12345678;
    TEST_ASSERT_EQ(cw_rol(val32, 4), 0x23456781);
    TEST_ASSERT_EQ(cw_ror(val32, 4), 0x81234567);
    TEST_ASSERT_EQ(cw_rol(val32, 0), val32);
    TEST_ASSERT_EQ(cw_ror(val32, 0), val32);

    /* Saturating arithmetic */
    TEST_ASSERT_EQ(cw_sat_add((uint32_t) 10, (uint32_t) 20), 30);
    TEST_ASSERT_EQ(cw_sat_add((uint32_t) (UINT32_MAX - 5), (uint32_t) 10),
                   UINT32_MAX);
    TEST_ASSERT_EQ(cw_sat_sub((uint32_t) 10, (uint32_t) 20), 0);
    TEST_ASSERT_EQ(cw_sat_sub((uint32_t) 20, (uint32_t) 10), 10);

    TEST_ASSERT_EQ(cw_sat_add((int32_t) (INT32_MAX - 5), (int32_t) 10),
                   INT32_MAX);
    TEST_ASSERT_EQ(cw_sat_add((int32_t) (INT32_MIN + 5), (int32_t) -10),
                   INT32_MIN);
    TEST_ASSERT_EQ(cw_sat_sub((int32_t) (INT32_MIN + 5), (int32_t) 10),
                   INT32_MIN);
    TEST_ASSERT_EQ(cw_sat_sub((int32_t) (INT32_MAX - 5), (int32_t) -10),
                   INT32_MAX);

    return TEST_SUCCESS;
}
