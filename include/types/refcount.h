/* @title: Reference count */
#pragma once
#include <atomic.h>
#include <console/panic.h>
#include <console/printf.h>
#include <enforce.h>
#include <limits.h>
#include <stdbool.h>
#include <types/types.h>

static inline void refcount_init(refcount_t *rc, uint32_t val) {
    atomic_store(rc, val);
}

static inline bool refcount_inc(refcount_t *rc) {
    while (true) {
        uint32_t old = atomic_load(rc);
        if (old == UINT_MAX)
            return false;

        uint32_t expected = old;
        if (atomic_cas_weak(rc, &expected, old + 1))
            return true;
    }
}

static inline bool refcount_inc_not_zero(refcount_t *rc) {
    while (true) {
        uint32_t old = atomic_load(rc);
        if (old == 0)
            return false;

        uint32_t expected = old;
        if (atomic_cas_weak(rc, &expected, old + 1))
            return true;

        old = expected;
    }
}

static inline uint32_t refcount_read(refcount_t *rc) {
    return atomic_load(rc);
}

static inline bool refcount_dec_and_test(refcount_t *rc) {
    while (true) {
        uint32_t old = atomic_load(rc);
        if (old == 0) {
            panic("possible UAF");
            return false;
        }

        uint32_t expected = old;
        if (atomic_cas_weak(rc, &expected, old - 1))
            return (old - 1) == 0;

        old = expected;
    }
}

/* drop a reference that is known not to be the last one */
static inline void refcount_put_not_last(refcount_t *rc) {
    enforce(!refcount_dec_and_test(rc));
}

/* generate a _getref function for a structure type, given a refcount
 * member name and a failure condition. if the failure condition (e.g. object
 * being cleaned up) is met, then the _getref will return false. will panic if
 * refcount drops to zero before we get a chance to decrement it.
 */
#define REFCOUNT_GENERATE_GETREF_FOR_STRUCT_WITH_FAILURE_COND(                 \
    __struct, __refcount_member, __failure_member, __failure_state)            \
    static inline bool __struct##_getref(struct __struct *obj) {               \
        uint32_t old;                                                          \
        while (true) {                                                         \
            old = atomic_load_acq(&obj->__refcount_member);                    \
                                                                               \
            /* panic if refcount is zero (possible UAF) */                     \
            if (!old) {                                                        \
                panic("possible UAF");                                         \
            }                                                                  \
                                                                               \
            /* check failure before attempting to increment */                 \
            if (atomic_load_acq(&obj->__failure_member) __failure_state) {     \
                return false;                                                  \
            }                                                                  \
                                                                               \
            /* attempt to increment refcount */                                \
            if (atomic_cas_weak(&obj->__refcount_member, &old, old + 1,        \
                                mo_acquire, mo_rlx)) {                         \
                return true;                                                   \
            }                                                                  \
                                                                               \
            cpu_pause();                                                       \
        }                                                                      \
    }

#define REFCOUNT_ASSERT_ZERO(rc) (enforce(atomic_load(&rc) == 0))
