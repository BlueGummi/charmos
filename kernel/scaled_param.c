#include <global.h>
#include <kassert.h>
#include <math/clamp.h>
#include <math/fixed.h>
#include <math/fixed_extended.h>
#include <math/range.h>
#include <scaled_param.h>
#include <smp/core.h>
#include <string.h>

static size_t geom_interp(size_t a, size_t b, fx32_32_t t) {
    if (t <= 0)
        return a;
    if (t >= FX_ONE)
        return b;
    if (a == b)
        return a;

    fx32_32_t ln_a = fx_ln(fx_from_int((int64_t) a));
    fx32_32_t ln_b = fx_ln(fx_from_int((int64_t) b));
    fx32_32_t result = fx_exp(fx_add(ln_a, fx_mul(t, fx_sub(ln_b, ln_a))));

    int64_t val = fx_to_int(result);
    CLAMP(val, (int64_t) a, (int64_t) b);
    return (size_t) val;
}

static size_t linear_interp(size_t a, size_t b, fx32_32_t t) {
    if (t <= 0)
        return a;
    if (t >= FX_ONE)
        return b;
    if (a == b)
        return a;

    fx32_32_t span = fx_from_int((int64_t) b - (int64_t) a);
    int64_t val = fx_to_int(fx_add(fx_from_int((int64_t) a), fx_mul(t, span)));
    CLAMP(val, (int64_t) a, (int64_t) b);
    return (size_t) val;
}

static size_t scale_log(fx32_32_t value, size_t min_val, size_t def_val,
                        size_t max_val) {
    value = fx_clamp(value, 0, FX_ONE);
    if (value == FX_HALF)
        return def_val;
    if (value < FX_HALF)
        return geom_interp(min_val, def_val, value << 1);
    return geom_interp(def_val, max_val, (value - FX_HALF) << 1);
}

static size_t scale_linear(fx32_32_t value, size_t min_val, size_t def_val,
                           size_t max_val) {
    value = fx_clamp(value, 0, FX_ONE);
    if (value == FX_HALF)
        return def_val;
    if (value < FX_HALF)
        return linear_interp(min_val, def_val, value << 1);
    return linear_interp(def_val, max_val, (value - FX_HALF) << 1);
}

static size_t scale_core_multiplier(fx32_32_t value, size_t min_val,
                                    size_t def_val, size_t max_val) {
    size_t per_core = scale_linear(value, min_val, def_val, max_val);
    return per_core * (global.core_count > 0 ? global.core_count : 1);
}

static const scaling_fn_t scale_lut[SCALE_CURVE_MAX] = {
    [SCALE_NONE] = NULL,
    [SCALE_PIECEWISE_LOG] = scale_log,
    [SCALE_PIECEWISE_LINEAR] = scale_linear,
    [SCALE_CORE_MULTIPLIER] = scale_core_multiplier,
    [SCALE_CUSTOM] = NULL,
};

size_t scaled_param_eval(const struct scaled_param *desc, fx32_32_t value) {
    if (!desc || desc->curve == SCALE_NONE)
        return 0;

    if (desc->curve == SCALE_CUSTOM)
        return desc->custom_scale
                   ? desc->custom_scale(value, desc->min_val, desc->def_val,
                                        desc->max_val)
                   : desc->def_val;

    kassert(IN_RANGE(desc->curve, SCALE_NONE, SCALE_CUSTOM));
    scaling_fn_t fn = scale_lut[desc->curve];
    return fn ? fn(value, desc->min_val, desc->def_val, desc->max_val)
              : desc->def_val;
}

size_t scaled_param_format(const struct scaled_param *desc, fx32_32_t value,
                           size_t scaled_val, char *buf, size_t cap) {
    if (!desc || desc->curve == SCALE_NONE)
        return 0;
    if (desc->custom_print)
        return desc->custom_print(value, scaled_val, buf, cap);
    return snprintf(buf, cap, "%zu %s", scaled_val,
                    desc->unit ? desc->unit : "units");
}
