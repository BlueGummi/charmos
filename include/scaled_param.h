/* @title: Scaled parameters */
#pragma once
#include <math/fixed.h>
#include <stddef.h>
#include <stdint.h>

enum scale_curve : uint8_t {
    SCALE_NONE = 0,
    SCALE_PIECEWISE_LOG,
    SCALE_PIECEWISE_LINEAR,
    SCALE_CORE_MULTIPLIER,
    SCALE_CUSTOM,
    SCALE_CURVE_MAX,
};

typedef size_t (*scaling_fn_t)(fx32_32_t value, size_t min_val, size_t def_val,
                               size_t max_val);

typedef size_t (*scaled_param_print_fn_t)(fx32_32_t value, size_t scaled_val,
                                          char *buf, size_t cap);

struct scaled_param {
    enum scale_curve curve;
    size_t min_val;
    size_t def_val;
    size_t max_val;
    const char *unit;

    scaling_fn_t custom_scale;
    scaled_param_print_fn_t custom_print;
};

size_t scaled_param_eval(const struct scaled_param *desc, fx32_32_t value);
size_t scaled_param_format(const struct scaled_param *desc, fx32_32_t value,
                           size_t scaled_val, char *buf, size_t cap);
