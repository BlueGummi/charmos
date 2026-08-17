/* @title: Queued Spinlock (MCS-based 4-byte qspinlock) */
#pragma once
#include <asm.h>
#include <compiler.h>
#include <kassert.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* TODO: I would really like to use CNA lock cohorting as an extension to
 * qspinlock, possibly also with a boot-time flag to enable/disable it
 * for testing. Could be super interesting! */

/*
 * Bit layout of the lock word:
 *
 *  0 -  7: locked byte (Q_SPIN_LOCKED_VAL = 0x01)
 *  8     : pending bit (Q_SPIN_PENDING_VAL = 0x100)
 *  9 - 10: tail context index
 * 16 - 31: tail CPU ID + 1 (16 bits)
 */
#define Q_SPIN_LOCKED_OFFSET 0
#define Q_SPIN_LOCKED_BITS 8
#define Q_SPIN_LOCKED_MASK 0x000000FFU
#define Q_SPIN_LOCKED_VAL 0x00000001U

#define Q_SPIN_PENDING_OFFSET 8
#define Q_SPIN_PENDING_BITS 1
#define Q_SPIN_PENDING_MASK 0x00000100U
#define Q_SPIN_PENDING_VAL 0x00000100U

#define Q_SPIN_LOCKED_PENDING_MASK (Q_SPIN_LOCKED_MASK | Q_SPIN_PENDING_MASK)

#define Q_SPIN_TAIL_LVL_OFFSET 9
#define Q_SPIN_TAIL_LVL_BITS 2
#define Q_SPIN_TAIL_LVL_MASK 0x00000600U

#define Q_SPIN_TAIL_CPU_OFFSET 16
#define Q_SPIN_TAIL_CPU_BITS 16
#define Q_SPIN_TAIL_CPU_MASK 0xFFFF0000U

#define Q_SPIN_TAIL_MASK (Q_SPIN_TAIL_LVL_MASK | Q_SPIN_TAIL_CPU_MASK)

enum qspinlock_level {
    QSPINLOCK_LEVEL_NORMAL, /* prev irql = DISPATCH */
    QSPINLOCK_LEVEL_IRQ,    /* prev irql = HIGH */
    QSPINLOCK_LEVEL_NMI,    /* For later usage */
    QSPINLOCK_LEVEL_MAX,
};

struct qspinlock {
    _Atomic uint32_t val;
};

#define QSPINLOCK_INIT                                                         \
    (struct qspinlock) {                                                       \
        .val = ATOMIC_VAR_INIT(0)                                              \
    }

static inline void qspin_lock_init(struct qspinlock *lock) {
    atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
}

static inline bool __warn_unused_result
qspin_trylock_raw(struct qspinlock *lock) {
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        &lock->val, &expected, Q_SPIN_LOCKED_VAL, memory_order_acquire,
        memory_order_relaxed);
}

void qspin_lock_slowpath(struct qspinlock *lock, uint32_t val);

static inline void qspin_lock_raw(struct qspinlock *lock) {
    uint32_t val = 0;
    if (likely(atomic_compare_exchange_strong_explicit(
            &lock->val, &val, Q_SPIN_LOCKED_VAL, memory_order_acquire,
            memory_order_relaxed)))
        return;

    qspin_lock_slowpath(lock, val);
}

static inline void qspin_unlock_raw(struct qspinlock *lock) {
    /* Clear only the locked byte so pending and tail bits remain valid */
    atomic_store_explicit((_Atomic uint8_t *) &lock->val, 0,
                          memory_order_release);
}

static inline bool qspin_is_locked(const struct qspinlock *lock) {
    return (atomic_load_explicit(&lock->val, memory_order_relaxed) &
            Q_SPIN_LOCKED_MASK) != 0;
}

static inline bool qspin_held(const struct qspinlock *lock) {
    return qspin_is_locked(lock);
}

/*
 * IRQL-aware QSpinlock APIs
 */
static inline enum irql __warn_unused_result
qspin_lock(struct qspinlock *lock) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    qspin_lock_raw(lock);
    return irql;
}

static inline void qspin_unlock(struct qspinlock *lock, enum irql old_irql) {
    qspin_unlock_raw(lock);
    irql_lower(old_irql);
}

static inline enum irql __warn_unused_result
qspin_lock_irq_disable(struct qspinlock *lock) {
    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    qspin_lock_raw(lock);
    return irql;
}

static inline void qspin_unlock_irq_restore(struct qspinlock *lock,
                                            enum irql old_irql) {
    qspin_unlock_raw(lock);
    irql_lower(old_irql);
}

static inline bool __warn_unused_result qspin_trylock(struct qspinlock *lock,
                                                      enum irql *out) {
    *out = irql_raise(IRQL_DISPATCH_LEVEL);
    if (qspin_trylock_raw(lock))
        return true;

    irql_lower(*out);
    return false;
}

static inline bool __warn_unused_result
qspin_trylock_irq_disable(struct qspinlock *lock, enum irql *out) {
    *out = irql_raise(IRQL_HIGH_LEVEL);
    if (qspin_trylock_raw(lock))
        return true;

    irql_lower(*out);
    return false;
}

#define QSPINLOCK_ASSERT_HELD(l) kassert(qspin_is_locked(l))
