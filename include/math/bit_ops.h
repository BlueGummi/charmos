/* @title: Bit operations */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline size_t popcount(size_t n) {
    size_t count = 0;
    while (n > 0) {
        if (n & 1)
            count++;

        n >>= 1;
    }
    return count;
}

static inline size_t next_pow2(size_t x) {
    size_t p = 1;
    if (x == 0)
        return 1;
    while (p < x) {
        if (p > (SIZE_MAX >> 1))
            return p;
        p <<= 1;
    }
    return p;
}

static inline size_t prev_pow2(size_t x) {
    size_t p = 1;
    if (x == 0)
        return 1;
    while (p <= x) {
        if (p > (SIZE_MAX >> 1))
            return p;
        p <<= 1;
    }
    return p >> 1;
}
