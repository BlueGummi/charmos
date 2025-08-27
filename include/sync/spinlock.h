#pragma once
#include <asm.h>
#include <console/panic.h>
#include <mp/core.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>

struct spinlock {
    atomic_bool state;
};

#define SPINLOCK_INIT {ATOMIC_VAR_INIT(0)}

static inline void spinlock_init(struct spinlock *lock) {
    atomic_store(&lock->state, 0);
}

static inline void spin_lock_raw(struct spinlock *lock) {
    bool expected;
    do {
        expected = 0;
        while (atomic_load_explicit(&lock->state, memory_order_relaxed) != 0)
            cpu_relax();
    } while (!atomic_compare_exchange_weak_explicit(&lock->state, &expected, 1,
                                                    memory_order_acquire,
                                                    memory_order_relaxed));
}

static inline void spin_unlock_raw(struct spinlock *lock) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
}

static inline void spin_unlock(struct spinlock *lock, enum irql old) {
    atomic_exchange_explicit(&lock->state, 0, memory_order_release);
    irql_lower(old);
}

static inline enum irql spin_lock(struct spinlock *lock) {
    if (global.current_bootstage >= BOOTSTAGE_MID_MP && in_interrupt())
        k_panic("Attempted to take non-ISR safe spinlock from an ISR!\n");

    spin_lock_raw(lock);
    return irql_raise(IRQL_DISPATCH_LEVEL);
}

static inline enum irql spin_lock_irq_disable(struct spinlock *lock) {
    spin_lock_raw(lock);
    return irql_raise(IRQL_HIGH_LEVEL);
}

static inline bool spin_trylock(struct spinlock *lock) {
    bool expected = 0;
    return atomic_compare_exchange_strong_explicit(
        &lock->state, &expected, 1, memory_order_acquire, memory_order_relaxed);
}

/* Keep these static inline so you only "pay for what you need" (e.g. if you
 * never call trylock() you don't pay the cost of having that dead function
 * in the object file/binary) */

#define SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(type, member)                 \
    static inline enum irql type##_lock(struct type *obj) {                    \
        return spin_lock(&obj->member);                                        \
    }                                                                          \
                                                                               \
    static inline enum irql type##_lock_irq_disable(struct type *obj) {        \
        return spin_lock_irq_disable(&obj->member);                            \
    }                                                                          \
                                                                               \
    static inline void type##_unlock(struct type *obj, enum irql irql) {       \
        spin_unlock(&obj->member, irql);                                       \
    }                                                                          \
                                                                               \
    static inline bool type##_trylock(struct type *obj) {                      \
        return spin_trylock(&obj->member);                                     \
    }
