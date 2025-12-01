/* @title: Extended Fixed Point Arithmetic */
#pragma once
#include <math/fixed.h>

fx16_16_t fx_poly_eval(fx16_16_t x, const fx16_16_t *c, int n);
fx16_16_t fx_exp(fx16_16_t x);
fx16_16_t fx_log(fx16_16_t x);
fx16_16_t fx_sin(fx16_16_t angle);
fx16_16_t fx_cos(fx16_16_t angle);
void fx_sincos(fx16_16_t angle, fx16_16_t *sin_out, fx16_16_t *cos_out);
