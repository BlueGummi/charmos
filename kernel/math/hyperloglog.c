#include <math/fixed.h>
#include <math/hash.h>
#include <math/hyperloglog.h>
#include <mem/alloc.h>

/* Notes on naming: all HLL_ and hll_ prefixed functions are INTERNAL,
 * all hyperloglog_ and HYPERLOGLOG_ prefixed functions are EXTERNAL */

#define HLL_ALPHA_16 FX(0.673)
#define HLL_ALPHA_32 FX(0.697)
#define HLL_ALPHA_64 FX(0.709)

struct hyperloglog {
    uint8_t register_width;
    fx32_32_t alpha_mm;
    size_t nr_registers;
    uint8_t *registers;
};

static inline fx32_32_t hll_alpha_128_plus(size_t nr_registers) {
    fx32_32_t numerator = FX(0.7213);
    fx32_32_t denominator = fx_div(FX(2.079), fx_from_int(nr_registers));
    return fx_div(numerator, denominator);
}

struct hyperloglog *hyperloglog_create(uint8_t register_width) {
    size_t nr_registers = 1 << register_width;
    uint8_t *reg_storage = vnews(reg_storage, nr_registers);
    struct hyperloglog *hll_ret = vnew(hll_ret);

    if (!reg_storage || !hll_ret) {
        vfree(reg_storage);
        vfree(hll_ret);
        return NULL;
    }

    hll_ret->registers = reg_storage;
    hll_ret->nr_registers = nr_registers;
    hll_ret->register_width = register_width;

    fx32_32_t alpha;
    switch (nr_registers) {
    case 16: alpha = HLL_ALPHA_16; break;
    case 32: alpha = HLL_ALPHA_32; break;
    case 64: alpha = HLL_ALPHA_64; break;
    default: alpha = hll_alpha_128_plus(nr_registers); break;
    }

    hll_ret->alpha_mm = fx_mul(fx_mul(alpha, fx_from_int(nr_registers)),
                               fx_from_int(nr_registers));
    return hll_ret;
}

void hyperloglog_destroy(struct hyperloglog *hll) {
    vfree(hll->registers);
    vfree(hll);
}

void hyperloglog_add(struct hyperloglog *hll, uint32_t hash) {
    uint8_t rw = hll->register_width;

    uint32_t idx = hash >> (32U - rw);
    uint8_t rank = MIN(32U - hll->register_width, cw_clz(hash << rw)) + 1;

    hll->registers[idx] = MAX(rank, hll->registers[idx]);
}

fx32_32_t hyperloglog_estimate(struct hyperloglog *hll) {
    fx32_32_t sum = FX(0.0);
    size_t zero_registers = 0;

    for (size_t i = 0; i < hll->nr_registers; i++) {
        uint8_t val = hll->registers[i];
        if (val == 0)
            zero_registers++;

        /* 1.0 / 2^val */
        sum += fx_div(FX(1.0), fx_from_int(1ULL << val));
    }

    /* estimate E = alpha_m * m^2 / sum */
    fx32_32_t estimate = fx_div(hll->alpha_mm, sum);

    /* range correction: E <= (5/2) * m */
    fx32_32_t threshold_small = fx_mul(FX(2.5), fx_from_int(hll->nr_registers));
    if (estimate <= threshold_small) {
        if (zero_registers > 0) {
            /* linear counting: m * ln(m / V) */
            fx32_32_t m = fx_from_int(hll->nr_registers);
            fx32_32_t v = fx_from_int(zero_registers);
            estimate = fx_mul(m, fx_ln(fx_div(m, v)));
        }
        return estimate;
    }

    /* Large range correction: E > (1/30) * 2^32 */
    fx32_32_t threshold_large =
        FX(143165576.533); /* (1.0 / 30.0) * (1ULL << 32) */

    if (estimate > threshold_large) {
        /* -2^32 * ln(1 - E / 2^32) */
        fx32_32_t two_32 = fx_from_int(1ULL << 32);
        fx32_32_t ratio = fx_div(estimate, two_32);

        if (ratio < FX(1.0))
            estimate = fx_mul(-two_32, fx_ln(FX(1.0) - ratio));
    }

    return estimate;
}
