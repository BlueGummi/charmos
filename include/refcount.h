#include <console/printf.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <types.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#pragma once

typedef atomic_uint refcount_t;

static inline void refcount_init(refcount_t *rc, unsigned int val) {
    atomic_store(rc, val);
}

static inline bool refcount_inc(refcount_t *rc) {
    unsigned int old = atomic_load(rc);
    for (;;) {
        if (old == 0 || old == UINT_MAX)
            return false; // can't increment

        if (atomic_compare_exchange_weak(rc, &old, old + 1))
            return true;
        // `old` is updated by the intrinsic if the exchange fails
    }
}

static inline bool refcount_inc_not_zero(refcount_t *rc) {
    unsigned int old = atomic_load(rc);
    while (old != 0) {
        if (atomic_compare_exchange_weak(rc, &old, old + 1))
            return true;
    }
    return false;
}

static inline bool refcount_dec_and_test(refcount_t *rc) {
    unsigned int old = atomic_load(rc);
    for (;;) {
        if (old == 0)
            return false; // underflow prevented

        if (atomic_compare_exchange_weak(rc, &old, old - 1))
            return old == 1;
    }
}
