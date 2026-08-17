/* @title: Bit Spinlock */
#pragma once
#include <asm.h>
#include <compiler.h>
#include <kassert.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* bit_spinlock: not an actual struct, but a series of macros for locking any
 * single bit within a byte, word, dword, and qword with comptime bounds check
 */

#define BITLOCK_MASK(bit, ptr) ((typeof(*(ptr))) (1ULL << (bit)))

#define BITLOCK_CHECK(bit, ptr)                                                \
    do {                                                                       \
        static_assert(__builtin_constant_p(bit)                                \
                          ? ((size_t) (bit) < sizeof(*(ptr)) * 8)              \
                          : 1,                                                 \
                      "bit index exceeds type width");                         \
        kassert((size_t) (bit) < sizeof(*(ptr)) * 8,                           \
                "bit index out of bounds for pointer type");                   \
    } while (0)

#define bit_spin_is_locked_raw(bit, ptr)                                       \
    ({                                                                         \
        BITLOCK_CHECK(bit, ptr);                                               \
        typeof(*(ptr)) __m = BITLOCK_MASK(bit, ptr);                           \
        (atomic_load_explicit((_Atomic typeof(*(ptr)) *) (ptr),                \
                              memory_order_relaxed) &                          \
         __m) != 0;                                                            \
    })

#define bit_spin_trylock_raw(bit, ptr)                                         \
    ({                                                                         \
        BITLOCK_CHECK(bit, ptr);                                               \
        typeof(*(ptr)) __m = BITLOCK_MASK(bit, ptr);                           \
        typeof(*(ptr)) __old = atomic_load_explicit(                           \
            (_Atomic typeof(*(ptr)) *) (ptr), memory_order_relaxed);           \
        bool __acquired = false;                                               \
        if (!(__old & __m)) {                                                  \
            __acquired = atomic_compare_exchange_weak_explicit(                \
                (_Atomic typeof(*(ptr)) *) (ptr), &__old,                      \
                (typeof(*(ptr))) (__old | __m), memory_order_acquire,          \
                memory_order_relaxed);                                         \
        }                                                                      \
        __acquired;                                                            \
    })

#define bit_spin_lock_raw(bit, ptr)                                            \
    do {                                                                       \
        BITLOCK_CHECK(bit, ptr);                                               \
        typeof(*(ptr)) __m = BITLOCK_MASK(bit, ptr);                           \
        while (true) {                                                         \
            typeof(*(ptr)) __old = atomic_load_explicit(                       \
                (_Atomic typeof(*(ptr)) *) (ptr), memory_order_relaxed);       \
            if (__old & __m) {                                                 \
                cpu_relax();                                                   \
                continue;                                                      \
            }                                                                  \
            if (atomic_compare_exchange_weak_explicit(                         \
                    (_Atomic typeof(*(ptr)) *) (ptr), &__old,                  \
                    (typeof(*(ptr))) (__old | __m), memory_order_acquire,      \
                    memory_order_relaxed))                                     \
                break;                                                         \
            cpu_relax();                                                       \
        }                                                                      \
    } while (0)

#define bit_spin_unlock_raw(bit, ptr)                                          \
    do {                                                                       \
        BITLOCK_CHECK(bit, ptr);                                               \
        typeof(*(ptr)) __m = BITLOCK_MASK(bit, ptr);                           \
        typeof(*(ptr)) __not_m = (typeof(*(ptr))) (~__m);                      \
        kassert(atomic_fetch_and_explicit((_Atomic typeof(*(ptr)) *) (ptr),    \
                                          __not_m, memory_order_release) &     \
                    __m,                                                       \
                "bit spinlock unlock on unheld lock");                         \
    } while (0)

#define bit_spin_lock(bit, ptr)                                                \
    ({                                                                         \
        enum irql __old_irql = irql_raise(IRQL_DISPATCH_LEVEL);                \
        bit_spin_lock_raw(bit, ptr);                                           \
        __old_irql;                                                            \
    })

#define bit_spin_unlock(bit, ptr, old_irql)                                    \
    do {                                                                       \
        bit_spin_unlock_raw(bit, ptr);                                         \
        irql_lower(old_irql);                                                  \
    } while (0)

#define bit_spin_lock_irq_disable(bit, ptr)                                    \
    ({                                                                         \
        enum irql __old_irql = irql_raise(IRQL_HIGH_LEVEL);                    \
        bit_spin_lock_raw(bit, ptr);                                           \
        __old_irql;                                                            \
    })

#define bit_spin_unlock_irq_restore(bit, ptr, old_irql)                        \
    do {                                                                       \
        bit_spin_unlock_raw(bit, ptr);                                         \
        irql_lower(old_irql);                                                  \
    } while (0)

#define bit_spin_trylock(bit, ptr, out_irql)                                   \
    ({                                                                         \
        *(out_irql) = irql_raise(IRQL_DISPATCH_LEVEL);                         \
        bool __ok = bit_spin_trylock_raw(bit, ptr);                            \
        if (!__ok)                                                             \
            irql_lower(*(out_irql));                                           \
        __ok;                                                                  \
    })

#define bit_spin_trylock_irq_disable(bit, ptr, out_irql)                       \
    ({                                                                         \
        *(out_irql) = irql_raise(IRQL_HIGH_LEVEL);                             \
        bool __ok = bit_spin_trylock_raw(bit, ptr);                            \
        if (!__ok)                                                             \
            irql_lower(*(out_irql));                                           \
        __ok;                                                                  \
    })

#define BIT_SPIN_LOCK_ASSERT_HELD(bit, ptr)                                    \
    kassert(bit_spin_is_locked_raw(bit, ptr), "bitlock not held")
