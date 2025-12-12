/* @title: Reference count */
#pragma once
#include <console/panic.h>
#include <console/printf.h>
#include <kassert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <types/types.h>

static inline void refcount_init(refcount_t *rc, unsigned int val) {
    atomic_store(rc, val);
}

static inline bool refcount_inc(refcount_t *rc) {
    while (true) {
        unsigned int old = atomic_load(rc);
        if (old == UINT_MAX)
            return false;

        unsigned int expected = old;
        if (atomic_compare_exchange_weak(rc, &expected, old + 1))
            return true;
    }
}

static inline bool refcount_inc_not_zero(refcount_t *rc) {
    while (true) {
        unsigned int old = atomic_load(rc);
        if (old == 0)
            return false;

        unsigned int expected = old;
        if (atomic_compare_exchange_weak(rc, &expected, old + 1))
            return true;

        old = expected;
    }
}

static inline uint32_t refcount_read(refcount_t *rc) {
    return atomic_load(rc);
}

static inline bool refcount_dec_and_test(refcount_t *rc) {
    while (true) {
        unsigned int old = atomic_load(rc);
        if (old == 0) {
            k_panic("%s(): Possible UAF!\n", __func__);
            return false;
        }

        unsigned int expected = old;
        if (atomic_compare_exchange_weak(rc, &expected, old - 1))
            return (old - 1) == 0;

        old = expected;
    }
}

#define REFCOUNT_ASSERT_ZERO(rc) (kassert(atomic_load(&rc) == 0))
