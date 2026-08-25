/* @title: Fixed Point Arithmetic */
#pragma once
#include <kassert.h>
#include <types/types.h>

#define FX_ONE ((fx32_32_t) (1LL << 32))
#define FX_HALF ((fx32_32_t) (1LL << 31))

/* Cast truncates towards zero, so we need to round */
#define FX(x) ((fx32_32_t) ((x) * 4294967296.0 + ((x) < 0 ? -0.5 : 0.5)))
#define FX_FROM_RATIO(n, d) ((fx32_32_t) (((__int128) (n) << 32) / (d)))

/* strtol style */
fx32_32_t fx_parse(const char *str, char **endptr);

static inline fx32_32_t fx_add(fx32_32_t a, fx32_32_t b) {
    return (fx32_32_t) (a + b);
}

static inline fx32_32_t fx_sub(fx32_32_t a, fx32_32_t b) {
    return (fx32_32_t) (a - b);
}

static inline fx32_32_t fx_mul(fx32_32_t a, fx32_32_t b) {
    bool neg = (a ^ b) < 0;
    uint64_t ua = (uint64_t) (a < 0 ? -a : a);
    uint64_t ub = (uint64_t) (b < 0 ? -b : b);

    uint64_t a_hi = ua >> 32;
    uint64_t a_lo = ua & 0xFFFFFFFFULL;
    uint64_t b_hi = ub >> 32;
    uint64_t b_lo = ub & 0xFFFFFFFFULL;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    uint64_t high = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);

    uint64_t q = (high << 32) | (mid & 0xFFFFFFFFULL);

    return (fx32_32_t) (neg ? -(int64_t) q : (int64_t) q);
}

static inline fx32_32_t fx_div(fx32_32_t a, fx32_32_t b) {
    kassert(b);
    bool neg = (a ^ b) < 0;
    uint64_t ua = (uint64_t) (a < 0 ? -a : a);
    uint64_t ub = (uint64_t) (b < 0 ? -b : b);

    uint64_t num_hi = ua >> 32;
    uint64_t num_lo = ua << 32;
    uint64_t q = 0;

    for (int i = 63; i >= 0; i--) {
        uint64_t bit = num_hi >> 63;
        num_hi = (num_hi << 1) | (num_lo >> 63);
        num_lo <<= 1;

        if (bit || num_hi >= ub) {
            num_hi -= ub;
            q |= (1ULL << i);
        }
    }

    return (fx32_32_t) (neg ? -(int64_t) q : (int64_t) q);
}

static inline fx32_32_t fx_from_int(int64_t x) {
    return x << 32;
}

static inline int64_t fx_to_int(fx32_32_t x) {
    return x >> 32;
}

static inline fx32_32_t fx_min(fx32_32_t a, fx32_32_t b) {
    return a < b ? a : b;
}

static inline fx32_32_t fx_max(fx32_32_t a, fx32_32_t b) {
    return a > b ? a : b;
}

static inline fx32_32_t fx_clamp(fx32_32_t x, fx32_32_t lo, fx32_32_t hi) {
    return fx_max(lo, fx_min(x, hi));
}

static inline fx32_32_t fx_pow_i32(fx32_32_t base, int exp) {
    fx32_32_t result = FX_ONE;
    if (exp < 0) {
        exp = -exp;
        base = fx_div(FX_ONE, base);
    }
    while (exp) {
        if (exp & 1)
            result = fx_mul(result, base);
        base = fx_mul(base, base);
        exp >>= 1;
    }
    return result;
}

static inline fx32_32_t fx_sqrt(fx32_32_t x) {
    if (x <= 0)
        return 0;
    fx32_32_t r = x;
    for (int i = 0; i < 8; i++) {
        r = (r + fx_div(x, (fx32_32_t) r)) >> 1;
    }
    return r;
}

/* Use two's complement to branchlessly floor for both signs */
static inline fx32_32_t fx_ceil(fx32_32_t x) {
    return (x + FX_ONE - 1) & ~(FX_ONE - 1);
}

static inline fx32_32_t fx_floor(fx32_32_t x) {
    return x & ~(FX_ONE - 1);
}

static inline fx32_32_t fx_map(fx32_32_t value, fx32_32_t from_low,
                               fx32_32_t from_high, fx32_32_t to_low,
                               fx32_32_t to_high) {
    fx32_32_t dist = value - from_low;
    fx32_32_t to_range = to_high - to_low;
    fx32_32_t from_range = from_high - from_low;
    fx32_32_t num = to_low + fx_mul(dist, to_range);
    return fx_div(num, from_range);
}

static inline fx32_32_t fx_lerp(fx32_32_t a, fx32_32_t b, fx32_32_t t) {
    return a + fx_mul(t, b - a);
}

static inline fx32_32_t fx_round_up(fx32_32_t x, fx32_32_t multiple) {
    return fx_mul(fx_div(x + multiple - FX_ONE, multiple), multiple);
}

static inline fx32_32_t fx_round_down(fx32_32_t x, fx32_32_t multiple) {
    return fx_mul(fx_div(x, multiple), multiple);
}
