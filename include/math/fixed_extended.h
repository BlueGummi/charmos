/* @title: Extended Fixed Point Arithmetic */
#pragma once
#include <math/fixed.h>

#define FP_PI 3.14159265358979323846
#define FX_PI FX(3.14159265358979323846)
#define FX_E FX(2.71828182845045235360)

fx32_32_t fx_poly_eval(fx32_32_t x, const fx32_32_t *c, int n);
fx32_32_t fx_exp(fx32_32_t x);
fx32_32_t fx_ln(fx32_32_t x);
fx32_32_t fx_sin(fx32_32_t angle);
fx32_32_t fx_cos(fx32_32_t angle);
void fx_sincos(fx32_32_t angle, fx32_32_t *sin_out, fx32_32_t *cos_out);
