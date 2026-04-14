/* @title: GCD and LCM */
#pragma once
#include <stddef.h>

static inline size_t gcd(size_t a, size_t b) {
    while (b) {
        size_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static inline size_t lcm(size_t a, size_t b) {
    return (a / gcd(a, b)) * b;
}
