#include <global.h>
#include <math/clamp.h>
#include <math/fixed.h>
#include <math/fixed_extended.h>
#include <math/isqrt.h>
#include <math/range.h>
#include <smp/core.h>
#include <string.h>
#include <test/test.h>

/* Geometric interpolation:
 * a * (b / a)^t = exp(ln(a) + t * (ln(b) - ln(a))) */
static inline size_t fx_geom_interp(size_t a, size_t b, fx32_32_t t) {
    if (t <= 0)
        return a;
    if (t >= FX_ONE)
        return b;
    if (a == b)
        return a;

    fx32_32_t ln_a = fx_ln(fx_from_int((int64_t) a));
    fx32_32_t ln_b = fx_ln(fx_from_int((int64_t) b));
    fx32_32_t ln_diff = fx_sub(ln_b, ln_a);
    fx32_32_t ln_target = fx_add(ln_a, fx_mul(t, ln_diff));
    fx32_32_t result = fx_exp(ln_target);

    int64_t val = fx_to_int(result);
    CLAMP(val, (int64_t) a, (int64_t) b);
    return (size_t) val;
}

/* Linear interpolation: a + t * (b - a) */
static inline size_t fx_linear_interp(size_t a, size_t b, fx32_32_t t) {
    if (t <= 0)
        return a;
    if (t >= FX_ONE)
        return b;
    if (a == b)
        return a;

    fx32_32_t span = fx_from_int((int64_t) b - (int64_t) a);
    fx32_32_t result = fx_add(fx_from_int((int64_t) a), fx_mul(t, span));

    int64_t val = fx_to_int(result);
    if (val < (int64_t) a)
        val = (int64_t) a;
    if (val > (int64_t) b)
        val = (int64_t) b;
    return (size_t) val;
}

/* Piecewise Logarithmic: [min, def] on [0.0, 0.5], [def, max] on [0.5, 1.0] */
size_t test_scale_log(fx32_32_t intensity, size_t min_val, size_t def_val,
                      size_t max_val) {
    intensity = fx_clamp(intensity, 0, FX_ONE);

    if (intensity == FX_HALF) {
        return def_val;
    } else if (intensity < FX_HALF) {
        fx32_32_t t = intensity << 1; /* [0.0, 0.5] -> [0.0, 1.0] */
        return fx_geom_interp(min_val, def_val, t);
    } else {
        fx32_32_t t = (intensity - FX_HALF) << 1; /* [0.5, 1.0] -> [0.0, 1.0] */
        return fx_geom_interp(def_val, max_val, t);
    }
}

/* Piecewise Linear: [min, def] on [0.0, 0.5], [def, max] on [0.5, 1.0] */
size_t test_scale_linear(fx32_32_t intensity, size_t min_val, size_t def_val,
                         size_t max_val) {
    intensity = fx_clamp(intensity, 0, FX_ONE);

    if (intensity == FX_HALF) {
        return def_val;
    } else if (intensity < FX_HALF) {
        fx32_32_t t = intensity << 1;
        return fx_linear_interp(min_val, def_val, t);
    } else {
        fx32_32_t t = (intensity - FX_HALF) << 1;
        return fx_linear_interp(def_val, max_val, t);
    }
}

/* Scale linearly with CPUs */
size_t test_scale_core_mult(fx32_32_t intensity, size_t min_val, size_t def_val,
                            size_t max_val) {
    size_t per_core = test_scale_linear(intensity, min_val, def_val, max_val);
    uint32_t cores = global.core_count > 0 ? global.core_count : 1;
    return per_core * cores;
}

static const test_scaling_fn_t test_scale_lut[TEST_SCALE_CURVE_MAX] = {
    [TEST_SCALE_NONE] = NULL,
    [TEST_SCALE_PIECEWISE_LOG] = test_scale_log,
    [TEST_SCALE_PIECEWISE_LINEAR] = test_scale_linear,
    [TEST_SCALE_CORE_MULTIPLIER] = test_scale_core_mult,
    [TEST_SCALE_CUSTOM] = NULL,
};

size_t test_intensity_eval(const struct test_intensity *desc,
                           fx32_32_t intensity) {
    if (!desc || desc->curve == TEST_SCALE_NONE)
        return 0;

    if (desc->curve == TEST_SCALE_CUSTOM) {
        if (desc->custom_scale)
            return desc->custom_scale(intensity, desc->min_val, desc->def_val,
                                      desc->max_val);
        return desc->def_val;
    }

    kassert(IN_RANGE(desc->curve, TEST_SCALE_NONE, TEST_SCALE_CUSTOM));
    test_scaling_fn_t fn = test_scale_lut[desc->curve];
    if (fn)
        return fn(intensity, desc->min_val, desc->def_val, desc->max_val);

    return desc->def_val;
}

size_t test_intensity_format(const struct test_intensity *desc,
                             fx32_32_t intensity, size_t scaled_val, char *buf,
                             size_t cap) {
    if (!desc || desc->curve == TEST_SCALE_NONE)
        return 0;

    if (desc->custom_print)
        return desc->custom_print(intensity, scaled_val, buf, cap);

    const char *unit = desc->unit ? desc->unit : "units";
    return snprintf(buf, cap, "%zu %s", scaled_val, unit);
}
