/* @title: Integer Square Root */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline uint64_t i64sqrt(uint64_t n) {
    if (n == 0)
        return 0;

    uint64_t x0 = n / 2;
    if (x0 == 0)
        return 1;

    uint64_t x1 = (x0 + n / x0) / 2;
    while (x1 < x0) {
        x0 = x1;
        x1 = (x0 + n / x0) / 2;
    }

    return x0;
}
