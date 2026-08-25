/* @title: Exponentially Weighted Moving Averages */
#pragma once
#include <math/fixed.h>
#include <stddef.h>
#include <stdint.h>

struct ewma {
    fx32_32_t alpha;
    fx32_32_t ewma;
};

static inline void ewma_init(struct ewma *e, fx32_32_t alpha) {
    e->alpha = alpha;
    e->ewma = FX(0.0);
}

static inline fx32_32_t ewma_update(struct ewma *e, size_t new) {
    fx32_32_t p1 = fx_mul(e->ewma, FX_ONE - e->alpha);
    fx32_32_t p2 = fx_mul(fx_from_int(new), e->alpha);
    return (e->ewma = p1 + p2);
}
