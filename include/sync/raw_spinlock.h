/* @title: Raw Spinlock */
#pragma once
#include <asm.h>
#include <atomic.h>
#include <compiler/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/lock_general.h>

struct TSA_CAPABILITY("spinlock") raw_spinlock {
    atomic_uint8_t state;
};

#define RAW_SPINLOCK_INIT ((struct raw_spinlock) {.state = ATOMIC_VAR_INIT(0)})
#define RAW_SPINLOCK_DEFINE(id) struct raw_spinlock id = RAW_SPINLOCK_INIT

static inline void raw_spinlock_init(struct raw_spinlock *lock) {
    atomic_store_rlx(&lock->state, 0);
}

static inline cc_warn_unused_result bool
raw_spin_trylock(struct raw_spinlock *lock)
    TSA_TRY_ACQUIRES(true, lock) TSA_NO_ANALYSIS {
    uint8_t expected = 0;
    return atomic_cas_strong(&lock->state, &expected, 1, mo_acquire, mo_rlx);
}

static inline void raw_spin_lock(struct raw_spinlock *lock)
    TSA_ACQUIRES(lock) TSA_NO_ANALYSIS {
    while (true) {
        if (raw_spin_trylock(lock))
            return;

        while (atomic_load_rlx(&lock->state) != 0)
            cpu_pause();
    }
}

static inline void raw_spin_unlock(struct raw_spinlock *lock)
    TSA_RELEASES(lock) TSA_NO_ANALYSIS {
    atomic_store_release(&lock->state, 0);
}

/* whether interrupts were enabled on entry */
static inline cc_warn_unused_result bool
raw_spin_lock_high(struct raw_spinlock *lock)
    TSA_ACQUIRES_IRQS_LOCK(lock) TSA_NO_ANALYSIS {
    bool irqs_were_enabled = irq_disable_save();
    raw_spin_lock(lock);
    return irqs_were_enabled;
}

static inline void raw_spin_unlock_irq_restore(struct raw_spinlock *lock,
                                               bool irqs_were_enabled)
    TSA_RELEASES_IRQS_LOCK(lock) TSA_NO_ANALYSIS {
    raw_spin_unlock(lock);
    irq_restore(irqs_were_enabled);
}

struct raw_spinlock_high_guard {
    struct raw_spinlock *lock;
    bool irqs_were_enabled;
};

static inline cc_always_inline cc_maybe_unused void
raw_spin_guard_assume(struct raw_spinlock *lock) TSA_ASSERT_CAPABILITY(lock) {
    cc_unused(lock);
}

static inline cc_always_inline cc_maybe_unused void
raw_spin_guard_exit(struct raw_spinlock **lock) TSA_NO_ANALYSIS {
    if (*lock)
        raw_spin_unlock(*lock);
}

static inline cc_always_inline cc_maybe_unused struct raw_spinlock *
raw_spin_guard_enter(struct raw_spinlock *lock) TSA_NO_ANALYSIS {
    raw_spin_lock(lock);
    return lock;
}

static inline cc_always_inline cc_maybe_unused void raw_spin_high_guard_exit(
    struct raw_spinlock_high_guard *g) TSA_RELEASES_IRQS TSA_NO_ANALYSIS {
    if (g->lock)
        raw_spin_unlock_irq_restore(g->lock, g->irqs_were_enabled);
}

static inline cc_always_inline cc_maybe_unused struct raw_spinlock_high_guard
raw_spin_high_guard_enter(struct raw_spinlock *lock)
    TSA_ACQUIRES_IRQS TSA_NO_ANALYSIS {
    return (struct raw_spinlock_high_guard) {
        .lock = lock,
        .irqs_were_enabled = raw_spin_lock_high(lock),
    };
}

#define raw_spin_guard(lock_)                                                  \
    cc_cleanup(raw_spin_guard_exit) struct raw_spinlock *PP_CONCAT(            \
        raw_spin_guard_, __COUNTER__) =                                        \
        (raw_spin_guard_assume(lock_), raw_spin_guard_enter(lock_))

#define raw_spin_guard_high(lock_)                                             \
    cc_cleanup(raw_spin_high_guard_exit) struct raw_spinlock_high_guard        \
    PP_CONCAT(raw_spin_guard_high_, __COUNTER__) =                             \
        (raw_spin_guard_assume(lock_), raw_spin_high_guard_enter(lock_))
