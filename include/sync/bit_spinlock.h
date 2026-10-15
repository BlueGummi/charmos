/* @title: Bit Spinlock */
#pragma once
#include <asm.h>
#include <atomic.h>
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <enforce.h>
#include <math/bit.h>
#include <sch/irql.h>
#include <stdbool.h>
#include <stdint.h>

/* bit_spinlock: not an actual struct, but a series of macros for locking any
 * single bit within a byte, word, dword, and qword with comptime bounds check
 */

#define BIT_SPINLOCK_MASK(bit, ptr) ((typeof(*(ptr))) BIT(bit))

#define BIT_SPINLOCK_CHECK(bit, ptr)                                           \
    do {                                                                       \
        static_assert(                                                         \
            ci_constant_p(bit) ? ((size_t) (bit) < sizeof(*(ptr)) * 8) : 1,    \
            "bit index exceeds type width");                                   \
        enforce((size_t) (bit) < sizeof(*(ptr)) * 8,                           \
                "bit index out of bounds for pointer type");                   \
    } while (0)

#define bit_spin_is_locked_raw(bit, ptr)                                       \
    ({                                                                         \
        BIT_SPINLOCK_CHECK(bit, ptr);                                          \
        atomic_test_bit_rlx((atomic(typeof(*(ptr))) *) (ptr), bit);            \
    })

#define bit_spin_trylock_raw(bit, ptr)                                         \
    ({                                                                         \
        BIT_SPINLOCK_CHECK(bit, ptr);                                          \
        atomic(typeof(*(ptr))) *__bs_ptr = (atomic(typeof(*(ptr))) *) (ptr);   \
        typeof(*(ptr)) __bs_mask = BIT_SPINLOCK_MASK(bit, ptr);                \
        typeof(*(ptr)) __bs_old = atomic_load_rlx(__bs_ptr);                   \
        bool __bs_acquired = false;                                            \
        if (!(__bs_old & __bs_mask)) {                                         \
            __bs_acquired = atomic_cas_weak(                                   \
                __bs_ptr, &__bs_old, (typeof(*(ptr))) (__bs_old | __bs_mask),  \
                mo_acquire, mo_rlx);                                           \
        }                                                                      \
        __bs_acquired;                                                         \
    })

#define bit_spin_lock_raw(bit, ptr)                                            \
    do {                                                                       \
        BIT_SPINLOCK_CHECK(bit, ptr);                                          \
        atomic(typeof(*(ptr))) *__bs_ptr = (atomic(typeof(*(ptr))) *) (ptr);   \
        typeof(*(ptr)) __bs_mask = BIT_SPINLOCK_MASK(bit, ptr);                \
        while (true) {                                                         \
            typeof(*(ptr)) __bs_old = atomic_load_rlx(__bs_ptr);               \
            if (__bs_old & __bs_mask) {                                        \
                cpu_pause();                                                   \
                continue;                                                      \
            }                                                                  \
            if (atomic_cas_weak(__bs_ptr, &__bs_old,                           \
                                (typeof(*(ptr))) (__bs_old | __bs_mask),       \
                                mo_acquire, mo_rlx))                           \
                break;                                                         \
            cpu_pause();                                                       \
        }                                                                      \
    } while (0)

#define bit_spin_unlock_raw(bit, ptr)                                          \
    do {                                                                       \
        BIT_SPINLOCK_CHECK(bit, ptr);                                          \
        enforce(atomic_test_and_clear_bit_release(                             \
                    (atomic(typeof(*(ptr))) *) (ptr), bit),                    \
                "bit spinlock unlock on unheld lock");                         \
    } while (0)

#define bit_spin_lock(bit, ptr)                                                \
    ({                                                                         \
        enum irql __bs_old_irql = irql_raise(IRQL_DISPATCH_LEVEL);             \
        bit_spin_lock_raw(bit, ptr);                                           \
        __bs_old_irql;                                                         \
    })

#define bit_spin_unlock(bit, ptr, old_irql)                                    \
    do {                                                                       \
        bit_spin_unlock_raw(bit, ptr);                                         \
        irql_lower(old_irql);                                                  \
    } while (0)

#define bit_spin_lock_high(bit, ptr)                                           \
    ({                                                                         \
        enum irql __bs_old_irql = irql_raise(IRQL_HIGH_LEVEL);                 \
        bit_spin_lock_raw(bit, ptr);                                           \
        __bs_old_irql;                                                         \
    })

#define bit_spin_unlock_irq_restore(bit, ptr, old_irql)                        \
    do {                                                                       \
        bit_spin_unlock_raw(bit, ptr);                                         \
        irql_lower(old_irql);                                                  \
    } while (0)

#define bit_spin_trylock(bit, ptr, out_irql)                                   \
    ({                                                                         \
        enum irql *__bs_out_irql = (out_irql);                                 \
        *__bs_out_irql = irql_raise(IRQL_DISPATCH_LEVEL);                      \
        bool __bs_ok = bit_spin_trylock_raw(bit, ptr);                         \
        if (!__bs_ok)                                                          \
            irql_lower(*__bs_out_irql);                                        \
        __bs_ok;                                                               \
    })

#define bit_spin_trylock_high(bit, ptr, out_irql)                              \
    ({                                                                         \
        enum irql *__bs_out_irql = (out_irql);                                 \
        *__bs_out_irql = irql_raise(IRQL_HIGH_LEVEL);                          \
        bool __bs_ok = bit_spin_trylock_raw(bit, ptr);                         \
        if (!__bs_ok)                                                          \
            irql_lower(*__bs_out_irql);                                        \
        __bs_ok;                                                               \
    })

#define BIT_SPIN_LOCK_ASSERT_HELD(bit, ptr)                                    \
    enforce(bit_spin_is_locked_raw(bit, ptr), "bitlock not held")

struct bit_spinlock_guard {
    void *ptr;
    uint8_t bit;
    uint8_t size;
    enum irql irql;
};

struct bit_spinlock_raw_guard {
    void *ptr;
    uint8_t bit;
    uint8_t size;
};

static inline cc_always_inline void
bit_spin_unlock_sized_raw(uint8_t bit, void *ptr, uint8_t size) {
    switch (size) {
    case 1: bit_spin_unlock_raw(bit, (uint8_t *) ptr); break;
    case 2: bit_spin_unlock_raw(bit, (uint16_t *) ptr); break;
    case 4: bit_spin_unlock_raw(bit, (uint32_t *) ptr); break;
    case 8:
    default: bit_spin_unlock_raw(bit, (uint64_t *) ptr); break;
    }
}

static inline cc_always_inline cc_unused void
bit_spin_raw_guard_exit(struct bit_spinlock_raw_guard *g) {
    if (g->ptr)
        bit_spin_unlock_sized_raw(g->bit, g->ptr, g->size);
}

static inline cc_always_inline cc_unused void
bit_spin_guard_exit(struct bit_spinlock_guard *g)
    TSA_RELEASES(IRQL_RAISED) TSA_NO_ANALYSIS {
    if (g->ptr) {
        bit_spin_unlock_sized_raw(g->bit, g->ptr, g->size);
        irql_lower(g->irql);
    }
}

#define bit_spin_guard(bit_, ptr_)                                             \
    cc_cleanup(bit_spin_guard_exit) struct bit_spinlock_guard PP_CONCAT(       \
        bit_spin_guard_, __COUNTER__) = {                                      \
        .ptr = (void *) (ptr_),                                                \
        .bit = (uint8_t) (bit_),                                               \
        .size = (uint8_t) sizeof(*(ptr_)),                                     \
        .irql = bit_spin_lock((bit_), (ptr_)),                                 \
    }

#define bit_spin_guard_high(bit_, ptr_)                                        \
    cc_cleanup(bit_spin_guard_exit) struct bit_spinlock_guard PP_CONCAT(       \
        bit_spin_guard_high_, __COUNTER__) = {                                 \
        .ptr = (void *) (ptr_),                                                \
        .bit = (uint8_t) (bit_),                                               \
        .size = (uint8_t) sizeof(*(ptr_)),                                     \
        .irql = bit_spin_lock_high((bit_), (ptr_)),                            \
    }

#define bit_spin_guard_raw(bit_, ptr_)                                         \
    bit_spin_lock_raw((bit_), (ptr_));                                         \
    cc_cleanup(bit_spin_raw_guard_exit) struct bit_spinlock_raw_guard          \
    PP_CONCAT(bit_spin_guard_raw_, __COUNTER__) = {                            \
        .ptr = (void *) (ptr_),                                                \
        .bit = (uint8_t) (bit_),                                               \
        .size = (uint8_t) sizeof(*(ptr_)),                                     \
    }
