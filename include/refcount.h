#pragma once
#include <console/printf.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <types.h>

static inline void refcount_init(refcount_t *rc, unsigned int val) {
    atomic_store(rc, val);
}

static inline bool refcount_inc(refcount_t *rc) {
    unsigned int old = atomic_load(rc);
    for (;;) {
        /* can't increment */
        if (old == 0 || old == UINT_MAX)
            return false;

        /* `old` is updated if the exchange fails */
        if (atomic_compare_exchange_weak(rc, &old, old + 1))
            return true;
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
            return false;

        if (atomic_compare_exchange_weak(rc, &old, old - 1))
            return old == 1;
    }
}
