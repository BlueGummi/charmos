/* @title: Fixed Point Arithmetic */
#pragma once
#include <types/types.h>

#define FX_ONE ((fx16_16_t) (1 << 16))
#define FX_HALF ((fx16_16_t) (1 << 15))
#define FX(x) ((fx16_16_t) ((x) * 65536.0 + 0.5))

static inline fx16_16_t fx_mul(fx16_16_t a, fx16_16_t b) {
    return (fx16_16_t) (((int64_t) a * b) >> 16);
}

static inline fx16_16_t fx_div(fx16_16_t a, fx16_16_t b) {
    return (fx16_16_t) (((int64_t) a << 16) / b);
}

static inline fx16_16_t fx_from_int(int32_t x) {
    return x << 16;
}
static inline int32_t fx_to_int(fx16_16_t x) {
    return x >> 16;
}

/* COMPILE TIME ONLY! */
static inline fx16_16_t fx_from_float(double v) {
    return FX(v);
}

static inline fx16_16_t fx_min(fx16_16_t a, fx16_16_t b) {
    return a < b ? a : b;
}
static inline fx16_16_t fx_max(fx16_16_t a, fx16_16_t b) {
    return a > b ? a : b;
}

static inline fx16_16_t fx_clamp(fx16_16_t x, fx16_16_t lo, fx16_16_t hi) {
    return fx_max(lo, fx_min(x, hi));
}

static inline fx16_16_t fx_pow_i32(fx16_16_t base, int exp) {
    fx16_16_t result = FX_ONE;

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

static inline fx16_16_t fx_sqrt(fx16_16_t x) {
    if (x <= 0)
        return 0;

    int64_t r = x;
    for (int i = 0; i < 6; i++) {
        r = (r + fx_div(x, (fx16_16_t) r)) >> 1;
    }
    return (fx16_16_t) r;
}
