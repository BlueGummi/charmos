#include <math/fixed.h>
#include <math/fixed_extended.h>

fx32_32_t fx_exp(fx32_32_t x) {
    int64_t k = fx_to_int(fx_mul(x, FX(1.44269504089)));
    fx32_32_t k_ln2 = fx_mul(fx_from_int(k), FX(0.69314718056));
    fx32_32_t r = x - k_ln2;
    static const fx32_32_t coeffs[] = {FX(1.0), FX(1.0), FX(0.5), FX(1.0 / 6.0),
                                       FX(1.0 / 24.0)};
    fx32_32_t poly = fx_poly_eval(r, coeffs, 5);
    if (k >= 0)
        return poly << k;
    else
        return poly >> -k;
}

fx32_32_t fx_poly_eval(fx32_32_t x, const fx32_32_t *c, int n) {
    fx32_32_t r = c[n - 1];
    for (int i = n - 2; i >= 0; i--) {
        r = fx_mul(r, x) + c[i];
    }
    return r;
}

static const fx32_32_t FX_LN2 = FX(0.69314718056);

fx32_32_t fx_ln(fx32_32_t x) {
    if (x <= 0)
        return INT64_MIN;
    int64_t k = 0;
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
    fx32_32_t num = x - FX(1.0);
    fx32_32_t den = x + FX(1.0);
    fx32_32_t z = fx_div(num, den);
    /* Polynomial: z + z^3/3 + z^5/5 */
    fx32_32_t z2 = fx_mul(z, z);
    fx32_32_t z3 = fx_mul(z2, z);
    fx32_32_t z5 = fx_mul(z3, z2);
    fx32_32_t series = z + fx_mul(z3, fx_div(FX(1.0), FX(3.0))) +
                       fx_mul(z5, fx_div(FX(1.0), FX(5.0)));
    fx32_32_t ln_m = fx_mul(series, FX(2.0));
    return ln_m + fx_mul(fx_from_int(k), FX_LN2);
}

static const fx32_32_t cordic_atan[] = {
    FX(0.7853981633974483), FX(0.4636476090008061), FX(0.2449786631268641),
    FX(0.1243549945467614), FX(0.0624188099959574), FX(0.0312398334302683),
    FX(0.0156237286204768), FX(0.0078123410601011), FX(0.0039062301319669),
    FX(0.0019531225164788), FX(0.0009765621895593), FX(0.0004882812111949),
    FX(0.0002441406201494), FX(0.0001220703118937), FX(0.0000610351561742),
    FX(0.0000305175781166), FX(0.0000152587890863), FX(0.0000076293945430),
    FX(0.0000038146972715), FX(0.0000019073486358), FX(0.0000009536743179),
    FX(0.0000004768371590), FX(0.0000002384185795), FX(0.0000001192092898),
    FX(0.0000000596046449), FX(0.0000000298023224), FX(0.0000000149011612),
    FX(0.0000000074505806), FX(0.0000000037252903), FX(0.0000000018626452),
    FX(0.0000000009313226), FX(0.0000000004656613),
};

static const fx32_32_t FX_CORDIC_K = FX(0.6072529350088814);

static void fx_cordic(fx32_32_t angle, fx32_32_t *out_sin, fx32_32_t *out_cos) {
    fx32_32_t x = FX_CORDIC_K;
    fx32_32_t y = 0;
    fx32_32_t z = angle;
    for (int i = 0; i < 32; i++) {
        fx32_32_t x_new, y_new;
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

fx32_32_t fx_sin(fx32_32_t angle) {
    fx32_32_t s, c;
    fx_cordic(angle, &s, &c);
    return s;
}

fx32_32_t fx_cos(fx32_32_t angle) {
    fx32_32_t s, c;
    fx_cordic(angle, &s, &c);
    return c;
}

void fx_sincos(fx32_32_t angle, fx32_32_t *sin_out, fx32_32_t *cos_out) {
    fx_cordic(angle, sin_out, cos_out);
}
