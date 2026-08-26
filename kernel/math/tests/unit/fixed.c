#include "../test_internal.h"

#ifdef TEST_FIXED
TEST_GROUP_DECLARE(fixed, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

/* Q32.32, [integer] [fraction], everything here is signed, so we try to test
 * those cases more because _mul/_div work on absolute values and reapply
 * sign at end, and _ceil/_floor round by masking */

#define FX_QUARTER (FX_ONE / 4)
#define FX_EPS ((fx32_32_t) 1)

static bool fx_near(fx32_32_t a, fx32_32_t b, fx32_32_t tol) {
    fx32_32_t d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

TEST_DECLARE_UNIT(fx_mul_identities, .group = TEST_GROUP(fixed)) {
    static const fx32_32_t vals[] = {
        0,        FX_ONE,    -FX_ONE,  FX_HALF,   -FX_HALF,
        FX(3.25), FX(-3.25), FX(1000), FX(-1000), FX(0.001),
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(vals); i++) {
        fx32_32_t x = vals[i];

        TEST_ASSERT(fx_mul(x, 0) == 0);
        TEST_ASSERT(fx_mul(0, x) == 0);
        TEST_ASSERT(fx_mul(x, FX_ONE) == x);
        TEST_ASSERT(fx_mul(FX_ONE, x) == x);
        TEST_ASSERT(fx_mul(x, -FX_ONE) == -x);

        for (size_t j = 0; j < TEST_ARRAY_LEN(vals); j++)
            TEST_ASSERT(fx_mul(x, vals[j]) == fx_mul(vals[j], x));
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_mul_standard, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_mul(FX_HALF, FX_HALF) == FX_QUARTER);
    TEST_ASSERT(fx_mul(FX(2.0), FX(3.0)) == FX(6.0));
    TEST_ASSERT(fx_mul(FX(0.25), FX(4.0)) == FX_ONE);

    /* 32x32 partial products are recombined via carry out of middle word */
    TEST_ASSERT(fx_mul(FX(1.5), FX(1.5)) == FX(2.25));
    TEST_ASSERT(fx_mul(FX(12345.5), FX(2.0)) == FX(24691.0));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_mul_signs, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_mul(FX(-2.0), FX(3.0)) == FX(-6.0));
    TEST_ASSERT(fx_mul(FX(2.0), FX(-3.0)) == FX(-6.0));
    TEST_ASSERT(fx_mul(FX(-2.0), FX(-3.0)) == FX(6.0));
    TEST_ASSERT(fx_mul(FX(-0.5), FX(-0.5)) == FX_QUARTER);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_div_identities, .group = TEST_GROUP(fixed)) {
    static const fx32_32_t vals[] = {
        FX_ONE, -FX_ONE, FX_HALF, -FX_HALF, FX(3.25), FX(-3.25), FX(1000),
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(vals); i++) {
        fx32_32_t x = vals[i];

        TEST_ASSERT(fx_div(x, x) == FX_ONE);
        TEST_ASSERT(fx_div(x, FX_ONE) == x);
        TEST_ASSERT(fx_div(0, x) == 0);
        TEST_ASSERT(fx_div(x, -x) == -FX_ONE);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_div_standard, .group = TEST_GROUP(fixed),
                  TEST_INTENSITY(16, 64, 1024)) {
    size_t iters = ctx->intensity_val ? ctx->intensity_val : 64;
    TEST_ASSERT(fx_div(FX_ONE, FX(2.0)) == FX_HALF);
    TEST_ASSERT(fx_div(FX(6.0), FX(3.0)) == FX(2.0));
    TEST_ASSERT(fx_div(FX_ONE, FX(4.0)) == FX_QUARTER);
    TEST_ASSERT(fx_div(FX(-1.0), FX(2.0)) == -FX_HALF);
    TEST_ASSERT(fx_div(FX(1.0), FX(-2.0)) == -FX_HALF);
    TEST_ASSERT(fx_div(FX(-1.0), FX(-2.0)) == FX_HALF);

    /* Round trip, allowing for truncation each of two ops contributes */
    for (int64_t d = 1; d <= (int64_t) iters; d++) {
        fx32_32_t q = fx_div(FX_ONE, fx_from_int(d));
        TEST_ASSERT(fx_near(fx_mul(q, fx_from_int(d)), FX_ONE, 64));
    }

    return TEST_SUCCESS;
}

/* Masking fractional bits rounds toward -inf both +/- */
TEST_DECLARE_UNIT(fx_floor_ceil_standard, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_floor(0) == 0);
    TEST_ASSERT(fx_ceil(0) == 0);

    TEST_ASSERT(fx_floor(FX_ONE) == FX_ONE);
    TEST_ASSERT(fx_ceil(FX_ONE) == FX_ONE);

    TEST_ASSERT(fx_floor(FX(1.5)) == FX_ONE);
    TEST_ASSERT(fx_ceil(FX(1.5)) == FX(2.0));

    TEST_ASSERT(fx_floor(FX_HALF) == 0);
    TEST_ASSERT(fx_ceil(FX_HALF) == FX_ONE);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_floor_ceil_negatives, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_floor(-FX_HALF) == -FX_ONE);
    TEST_ASSERT(fx_ceil(-FX_HALF) == 0);

    TEST_ASSERT(fx_floor(FX(-1.5)) == FX(-2.0));
    TEST_ASSERT(fx_ceil(FX(-1.5)) == FX(-1.0));

    TEST_ASSERT(fx_floor(FX(-2.0)) == FX(-2.0));
    TEST_ASSERT(fx_ceil(FX(-2.0)) == FX(-2.0));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_floor_ceil_invariants, .group = TEST_GROUP(fixed),
                  TEST_INTENSITY(4, 16, 256)) {
    int64_t bound = ctx->intensity_val ? (int64_t) (ctx->intensity_val / 2) : 8;
    if (bound == 0)
        bound = 1;
    for (int64_t n = -bound; n <= bound; n++) {
        for (int64_t f = 0; f < 4; f++) {
            fx32_32_t x = fx_from_int(n) + (fx32_32_t) (f * (FX_ONE / 4));

            fx32_32_t lo = fx_floor(x);
            fx32_32_t hi = fx_ceil(x);

            TEST_ASSERT(lo <= x && x <= hi);
            TEST_ASSERT((lo & (FX_ONE - 1)) == 0);
            TEST_ASSERT((hi & (FX_ONE - 1)) == 0);
            TEST_ASSERT(hi - lo == (f == 0 ? 0 : FX_ONE));
        }
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_pow_i32_standard, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_pow_i32(FX(2.0), 0) == FX_ONE);
    TEST_ASSERT(fx_pow_i32(FX(2.0), 1) == FX(2.0));
    TEST_ASSERT(fx_pow_i32(FX(2.0), 2) == FX(4.0));
    TEST_ASSERT(fx_pow_i32(FX(2.0), 10) == FX(1024.0));
    TEST_ASSERT(fx_pow_i32(FX(0.5), 2) == FX_QUARTER);

    TEST_ASSERT(fx_pow_i32(0, 0) == FX_ONE);
    TEST_ASSERT(fx_pow_i32(FX(-3.0), 0) == FX_ONE);

    TEST_ASSERT(fx_pow_i32(FX(-2.0), 2) == FX(4.0));
    TEST_ASSERT(fx_pow_i32(FX(-2.0), 3) == FX(-8.0));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_pow_i32_negative_exponent, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_pow_i32(FX(2.0), -1) == FX_HALF);
    TEST_ASSERT(fx_pow_i32(FX(2.0), -2) == FX_QUARTER);
    TEST_ASSERT(fx_near(fx_pow_i32(FX(4.0), -1), FX(0.25), FX_EPS));

    for (int e = 1; e <= 8; e++) {
        fx32_32_t pos = fx_pow_i32(FX(2.0), e);
        fx32_32_t neg = fx_pow_i32(FX(2.0), -e);
        TEST_ASSERT(fx_near(fx_mul(pos, neg), FX_ONE, 64));
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_sqrt_converges_when_small, .group = TEST_GROUP(fixed)) {
    static const int64_t roots[] = {1, 2, 3, 4, 5, 8, 10, 16};

    for (size_t i = 0; i < TEST_ARRAY_LEN(roots); i++) {
        int64_t r = roots[i];
        fx32_32_t got = fx_sqrt(fx_from_int(r * r));
        TEST_ASSERT(fx_near(got, fx_from_int(r), FX_ONE / 1024));
    }

    TEST_ASSERT(fx_sqrt(FX_QUARTER) == FX_HALF);
    TEST_ASSERT(fx_near(fx_sqrt(FX(2.0)), FX(1.41421356), FX_ONE / 1024));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_sqrt_edges, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_sqrt(0) == 0);
    TEST_ASSERT(fx_sqrt(-FX_ONE) == 0);
    TEST_ASSERT(fx_sqrt(FX_ONE) == FX_ONE);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_conversion_roundtrip, .group = TEST_GROUP(fixed),
                  TEST_INTENSITY(256, 1024, 65536)) {
    int64_t bound = ctx->intensity_val ? (int64_t) ctx->intensity_val : 1024;
    for (int64_t n = -bound; n <= bound; n++)
        TEST_ASSERT(fx_to_int(fx_from_int(n)) == n);

    /* fx_to_int truncates toward -inf, same as mask */
    TEST_ASSERT(fx_to_int(FX_HALF) == 0);
    TEST_ASSERT(fx_to_int(FX(1.9)) == 1);
    TEST_ASSERT(fx_to_int(-FX_HALF) == -1);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(fx_clamp_standard, .group = TEST_GROUP(fixed)) {
    TEST_ASSERT(fx_clamp(FX(5.0), 0, FX_ONE) == FX_ONE);
    TEST_ASSERT(fx_clamp(FX(-5.0), 0, FX_ONE) == 0);
    TEST_ASSERT(fx_clamp(FX_HALF, 0, FX_ONE) == FX_HALF);
    TEST_ASSERT(fx_clamp(0, 0, FX_ONE) == 0);
    TEST_ASSERT(fx_clamp(FX_ONE, 0, FX_ONE) == FX_ONE);

    return TEST_SUCCESS;
}

#endif
