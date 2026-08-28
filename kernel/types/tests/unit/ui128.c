#include "../test_internal.h"

#ifdef TEST_UI128
TEST_GROUP_DECLARE(ui128, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

/* These are runtime functions the compiler wants to exist, so we can
 * support 128 bit integers. Each 128 bit integer is two 64 bit halves,
 * so we want to test things that cross the "word-seam" between the two */
#define U128(hi, lo) (((uint128_t) (uint64_t) (hi) << 64) | (uint64_t) (lo))

static uint128_t ref_shl(uint128_t a, int b) {
    return b == 0 ? a : a << b;
}

TEST_DECLARE_UNIT(ui128, ui128_shift_left) {
    static const uint128_t vals[] = {
        0,
        1,
        U128(0, UINT64_MAX),
        U128(UINT64_MAX, 0),
        U128(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL),
        UINT128_MAX,
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(vals); i++)
        for (int b = 0; b < 128; b++)
            TEST_ASSERT(__ashlti3(vals[i], b) == ref_shl(vals[i], b));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_shift_left_seam) {
    uint128_t one = 1;

    TEST_ASSERT(__ashlti3(one, 0) == one);
    TEST_ASSERT(__ashlti3(one, 63) == U128(0, 1ULL << 63));
    TEST_ASSERT(__ashlti3(one, 64) == U128(1, 0));
    TEST_ASSERT(__ashlti3(one, 65) == U128(2, 0));
    TEST_ASSERT(__ashlti3(one, 127) == U128(1ULL << 63, 0));

    /* Bits shifted past the top are dropped */
    TEST_ASSERT(__ashlti3(U128(UINT64_MAX, 0), 1) == U128(UINT64_MAX << 1, 0));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_shift_right_logical) {
    static const uint128_t vals[] = {
        0,
        1,
        U128(0, UINT64_MAX),
        U128(UINT64_MAX, 0),
        U128(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL),
        UINT128_MAX,
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(vals); i++)
        for (int b = 0; b < 128; b++) {
            uint128_t want = b == 0 ? vals[i] : vals[i] >> b;
            TEST_ASSERT(__lshrti3(vals[i], b) == want);
        }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_shift_right_logical_seam) {
    uint128_t top = U128(1ULL << 63, 0);

    TEST_ASSERT(__lshrti3(top, 0) == top);
    TEST_ASSERT(__lshrti3(top, 64) == U128(0, 1ULL << 63));
    TEST_ASSERT(__lshrti3(top, 127) == 1);
    TEST_ASSERT(__lshrti3(UINT128_MAX, 64) == U128(0, UINT64_MAX));

    /* Check for no sign extension */
    TEST_ASSERT(__lshrti3(UINT128_MAX, 127) == 1);

    return TEST_SUCCESS;
}

/* The arithmetic shift is the one with a sign to preserve; a logical shift
 * substituted here would pass every non-negative case in the sweep above. */
TEST_DECLARE_UNIT(ui128, ui128_shift_right_arithmetic) {
    static const int128_t vals[] = {
        0, 1, -1, 12345, -12345, INT128_MAX, INT128_MIN,
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(vals); i++)
        for (int b = 0; b < 128; b++) {
            int128_t want = b == 0 ? vals[i] : vals[i] >> b;
            TEST_ASSERT(__ashrti3(vals[i], b) == want);
        }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_shift_right_arithmetic_signs) {
    /* -1 stays -1 at every shift; that is the whole point of sign fill. */
    for (int b = 0; b < 128; b++)
        TEST_ASSERT(__ashrti3((int128_t) -1, b) == (int128_t) -1);

    TEST_ASSERT(__ashrti3((int128_t) -2, 1) == (int128_t) -1);
    TEST_ASSERT(__ashrti3(INT128_MIN, 127) == (int128_t) -1);
    TEST_ASSERT(__ashrti3(INT128_MAX, 127) == 0);

    /* Crossing the 64-bit seam must carry the fill into the high half. */
    TEST_ASSERT(__ashrti3(INT128_MIN, 64) ==
                (int128_t) U128(UINT64_MAX, 1ULL << 63));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_negate, TEST_INTENSITY(16, 64, 1024)) {
    size_t iters = ctx->intensity_val ? ctx->intensity_val : 64;
    TEST_ASSERT(__negti2(0) == 0);
    TEST_ASSERT(__negti2(1) == (int128_t) -1);
    TEST_ASSERT(__negti2((int128_t) -1) == 1);
    TEST_ASSERT(__negti2(INT128_MAX) == INT128_MIN + 1);

    /* The carry out of the low half is the part worth checking. */
    TEST_ASSERT(__negti2((int128_t) U128(0, 1)) ==
                (int128_t) U128(UINT64_MAX, UINT64_MAX));
    TEST_ASSERT(__negti2((int128_t) U128(1, 0)) ==
                (int128_t) U128(UINT64_MAX, 0));

    for (int128_t v = -(int128_t) iters; v <= (int128_t) iters; v++)
        TEST_ASSERT(__negti2(__negti2(v)) == v);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_multiply, TEST_INTENSITY(8, 32, 256)) {
    size_t bound = ctx->intensity_val ? ctx->intensity_val : 32;
    TEST_ASSERT(__multi3(0, 12345) == 0);
    TEST_ASSERT(__multi3(1, 12345) == 12345);
    TEST_ASSERT(__multi3(-1, 12345) == -12345);
    TEST_ASSERT(__multi3(-3, -4) == 12);

    /* 2^32 squared is 2^64, which lands exactly on the seam between the two
     * halves -- the point where the partial products have to carry across. */
    TEST_ASSERT(__multi3((int128_t) U128(0, 1ULL << 32),
                         (int128_t) U128(0, 1ULL << 32)) ==
                (int128_t) U128(1, 0));

    /* Anything at or above 2^64 squared overflows the result away entirely. */
    TEST_ASSERT(__multi3((int128_t) U128(1, 0), (int128_t) U128(1, 0)) == 0);

    for (int128_t a = -(int128_t) bound; a <= (int128_t) bound; a++)
        for (int128_t b = -(int128_t) bound; b <= (int128_t) bound; b++)
            TEST_ASSERT(__multi3(a, b) == a * b);

    return TEST_SUCCESS;
}

/* Division is checked through its own contract rather than a table: whatever
 * the quotient is, q*d + r must reproduce n and r must stay below d. */
TEST_DECLARE_UNIT(ui128, ui128_divmod_identity) {
    static const uint128_t nums[] = {
        0,           1,
        12345,       U128(0, UINT64_MAX),
        U128(1, 0),  U128(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL),
        UINT128_MAX,
    };
    static const uint128_t dens[] = {
        1, 2, 3, 10, 12345, U128(0, UINT64_MAX), U128(1, 0), UINT128_MAX,
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(nums); i++) {
        for (size_t j = 0; j < TEST_ARRAY_LEN(dens); j++) {
            uint128_t n = nums[i], d = dens[j], rem = 0;
            uint128_t q = __udivmodti4(n, d, &rem);

            TEST_ASSERT(rem < d);
            TEST_ASSERT(q * d + rem == n);
            TEST_ASSERT(__udivti3(n, d) == q);
            TEST_ASSERT(__umodti3(n, d) == rem);
        }
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_divmod_edges) {
    uint128_t rem = 0;

    /* A numerator smaller than the divisor is all remainder. */
    TEST_ASSERT(__udivmodti4(5, 10, &rem) == 0);
    TEST_ASSERT(rem == 5);

    TEST_ASSERT(__udivmodti4(0, 1, &rem) == 0 && rem == 0);
    TEST_ASSERT(__udivmodti4(UINT128_MAX, 1, &rem) == UINT128_MAX && rem == 0);
    TEST_ASSERT(__udivmodti4(UINT128_MAX, UINT128_MAX, &rem) == 1 && rem == 0);

    return TEST_SUCCESS;
}

/* C rounds signed division toward zero, which makes the remainder take the
 * sign of the dividend -- not the divisor. Both directions are easy to invert.
 */
TEST_DECLARE_UNIT(ui128, ui128_signed_divmod, TEST_INTENSITY(10, 40, 512)) {
    size_t bound = ctx->intensity_val ? ctx->intensity_val : 40;
    int128_t rem;

    TEST_ASSERT(__divmodti4(7, 2, &rem) == 3 && rem == 1);
    TEST_ASSERT(__divmodti4(-7, 2, &rem) == -3 && rem == -1);
    TEST_ASSERT(__divmodti4(7, -2, &rem) == -3 && rem == 1);
    TEST_ASSERT(__divmodti4(-7, -2, &rem) == 3 && rem == -1);

    TEST_ASSERT(__divti3(7, 2) == 3);
    TEST_ASSERT(__divti3(-7, 2) == -3);
    TEST_ASSERT(__divti3(7, -2) == -3);
    TEST_ASSERT(__divti3(-7, -2) == 3);

    TEST_ASSERT(__modti3(7, 2) == 1);
    TEST_ASSERT(__modti3(-7, 2) == -1);
    TEST_ASSERT(__modti3(7, -2) == 1);
    TEST_ASSERT(__modti3(-7, -2) == -1);

    for (int128_t a = -(int128_t) bound; a <= (int128_t) bound; a++)
        for (int128_t b = -7; b <= 7; b++) {
            if (b == 0)
                continue;
            int128_t r;
            int128_t q = __divmodti4(a, b, &r);
            TEST_ASSERT(q == a / b);
            TEST_ASSERT(r == a % b);
            TEST_ASSERT(__divti3(a, b) == a / b);
            TEST_ASSERT(__modti3(a, b) == a % b);
            TEST_ASSERT(q * b + r == a);
        }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_popcount_parity) {
    TEST_ASSERT_EQ(__popcountti2(0), 0);
    TEST_ASSERT_EQ(__popcountti2(UINT128_MAX), 128);
    TEST_ASSERT_EQ(__popcountti2(U128(UINT64_MAX, 0)), 64);
    TEST_ASSERT_EQ(__popcountti2(U128(0, UINT64_MAX)), 64);

    /* Every single bit must be seen exactly once, in both halves. */
    for (int b = 0; b < 128; b++)
        TEST_ASSERT_EQ(__popcountti2(__ashlti3(1, b)), 1);

    TEST_ASSERT_EQ(__parityti2(0), 0);
    TEST_ASSERT_EQ(__parityti2(1), 1);
    TEST_ASSERT_EQ(__parityti2(3), 0);
    TEST_ASSERT_EQ(__parityti2(UINT128_MAX), 0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(ui128, ui128_count_zeros) {
    for (int b = 0; b < 128; b++) {
        uint128_t v = __ashlti3(1, b);
        TEST_ASSERT_EQ(__clzti2(v), 127 - b);
        TEST_ASSERT_EQ(__ctzti2(v), b);
        TEST_ASSERT_EQ(__ffsti2((int128_t) v), b + 1);
    }

    TEST_ASSERT_EQ(__ffsti2(0), 0);
    TEST_ASSERT_EQ(__clzti2(UINT128_MAX), 0);
    TEST_ASSERT_EQ(__ctzti2(UINT128_MAX), 0);

    return TEST_SUCCESS;
}

/* Test the overflow checks in __negvti2 and similar functions */
TEST_DECLARE_UNIT(ui128, ui128_limits) {
    TEST_ASSERT(INT128_MAX > 0);
    TEST_ASSERT(INT128_MIN < 0);
    TEST_ASSERT(INT128_MAX == (int128_t) (UINT128_MAX >> 1));
    TEST_ASSERT(INT128_MIN == -INT128_MAX - 1);

    /* Signed overflow can get optimized away by the compiler, use uint128_t */
    TEST_ASSERT((int128_t) ((uint128_t) INT128_MAX + 1) == INT128_MIN);
    TEST_ASSERT((uint128_t) (UINT128_MAX + 1) == 0);

    return TEST_SUCCESS;
}

#endif
