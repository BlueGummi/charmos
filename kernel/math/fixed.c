#include <math/fixed.h>
#include <math/fixed_extended.h>

fx16_16_t fx_exp(fx16_16_t x) {
    int k = fx_to_int(fx_mul(x, FX(1.44269504089))); // 1/ln(2)
    fx16_16_t k_ln2 = fx_mul(fx_from_int(k), FX(0.69314718056));
    fx16_16_t r = x - k_ln2;

    static const fx16_16_t coeffs[] = {FX(1.0), FX(1.0), FX(0.5), FX(1.0 / 6.0),
                                       FX(1.0 / 24.0)};
    fx16_16_t poly = fx_poly_eval(r, coeffs, 5);

    if (k >= 0)
        return poly << k;
    else
        return poly >> -k;
}

fx16_16_t fx_poly_eval(fx16_16_t x, const fx16_16_t *c, int n) {
    fx16_16_t r = c[n - 1];

    for (int i = n - 2; i >= 0; i--) {
        r = fx_mul(r, x) + c[i];
    }

    return r;
}

static const fx16_16_t FX_LN2 = FX(0.69314718056);

fx16_16_t fx_log(fx16_16_t x) {
    if (x <= 0)
        return INT32_MIN;

    int k = 0;

    /* Normalize x into [1,2) */
    while (x < FX(1.0)) {
        x <<= 1;
        k--;
    }
    while (x >= FX(2.0)) {
        x >>= 1;
        k++;
    }

    /* z = (x - 1) / (x + 1) */
    fx16_16_t num = x - FX(1.0);
    fx16_16_t den = x + FX(1.0);
    fx16_16_t z = fx_div(num, den);

    /* Polynomial: z + z^3/3 + z^5/5 */
    fx16_16_t z2 = fx_mul(z, z);
    fx16_16_t z3 = fx_mul(z2, z);
    fx16_16_t z5 = fx_mul(z3, z2);

    fx16_16_t series = z + fx_mul(z3, fx_div(FX(1.0), FX(3.0))) +
                       fx_mul(z5, fx_div(FX(1.0), FX(5.0)));

    fx16_16_t ln_m = fx_mul(series, FX(2.0));

    return ln_m + fx_mul(fx_from_int(k), FX_LN2);
}

static const fx16_16_t cordic_atan[] = {
    FX(0.7853981633974483), FX(0.4636476090008061), FX(0.2449786631268641),
    FX(0.1243549945467614), FX(0.0624188099959574), FX(0.0312398334302683),
    FX(0.0156237286204768), FX(0.0078123410601011), FX(0.0039062301319669),
    FX(0.0019531225164788), FX(0.0009765621895593), FX(0.0004882812111949),
    FX(0.0002441406201494), FX(0.0001220703118937), FX(0.0000610351561742),
    FX(0.0000305175781166)};

static const fx16_16_t FX_CORDIC_K = FX(0.6072529350088814);
static void fx_cordic(fx16_16_t angle, fx16_16_t *out_sin, fx16_16_t *out_cos) {
    fx16_16_t x = FX_CORDIC_K;
    fx16_16_t y = 0;
    fx16_16_t z = angle;

    for (int i = 0; i < 16; i++) {
        fx16_16_t x_new, y_new;

        if (z >= 0) {
            x_new = x - (y >> i);
            y_new = y + (x >> i);
            z -= cordic_atan[i];
        } else {
            x_new = x + (y >> i);
            y_new = y - (x >> i);
            z += cordic_atan[i];
        }
        x = x_new;
        y = y_new;
    }

    *out_sin = y;
    *out_cos = x;
}

fx16_16_t fx_sin(fx16_16_t angle) {
    fx16_16_t s, c;
    fx_cordic(angle, &s, &c);
    return s;
}

fx16_16_t fx_cos(fx16_16_t angle) {
    fx16_16_t s, c;
    fx_cordic(angle, &s, &c);
    return c;
}

void fx_sincos(fx16_16_t angle, fx16_16_t *sin_out, fx16_16_t *cos_out) {
    fx_cordic(angle, sin_out, cos_out);
}
