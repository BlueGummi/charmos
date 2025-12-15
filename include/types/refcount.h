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
            k_panic("possible UAF\n");
            return false;
        }

        unsigned int expected = old;
        if (atomic_compare_exchange_weak(rc, &expected, old - 1))
            return (old - 1) == 0;

        old = expected;
    }
}

/* generate a _get function for a structure type, given a refcount
 * member name and a failure condition. if the failure condition (e.g. object
 * being cleaned up) is met, then the _get will return false. will panic if
 * refcount drops to zero before we get a chance to decrement it.
 */
#define REFCOUNT_GENERATE_GET_FOR_STRUCT_WITH_FAILURE_COND(                    \
    __struct, __refcount_member, __failure_member, __failure_state)            \
    static inline bool __struct##_get(struct __struct *obj) {                  \
        uint32_t old;                                                          \
        while (true) {                                                         \
            old = atomic_load_explicit(&obj->__refcount_member,                \
                                       memory_order_acquire);                  \
            if (!old) {                                                        \
                k_panic("possible UAF\n");                                     \
            }                                                                  \
                                                                               \
            if (atomic_compare_exchange_weak_explicit(                         \
                    &obj->__refcount_member, &old, old + 1,                    \
                    memory_order_acquire, memory_order_relaxed)) {             \
                if (atomic_load_explicit(&obj->__failure_member,               \
                                         memory_order_acquire)                 \
                        __failure_state) {                                     \
                    atomic_fetch_sub_explicit(&obj->__refcount_member, 1,      \
                                              memory_order_release);           \
                    return false;                                              \
                }                                                              \
                return true;                                                   \
            }                                                                  \
            cpu_relax();                                                       \
        }                                                                      \
    }

#define REFCOUNT_ASSERT_ZERO(rc) (kassert(atomic_load(&rc) == 0))
