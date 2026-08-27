/* @title: Raw Spinlock */
#pragma once
#include <asm.h>
#include <compiler.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct raw_spinlock {
    _Atomic uint8_t state;
};

#define RAW_SPINLOCK_INIT ((struct raw_spinlock) {.state = ATOMIC_VAR_INIT(0)})
#define RAW_SPINLOCK_DEFINE(id) struct raw_spinlock id = RAW_SPINLOCK_INIT

static inline void raw_spinlock_init(struct raw_spinlock *lock) {
    atomic_store_explicit(&lock->state, 0, memory_order_relaxed);
}

static inline bool __warn_unused_result
raw_spin_trylock(struct raw_spinlock *lock) {
    uint8_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        &lock->state, &expected, 1, memory_order_acquire, memory_order_relaxed);
}

static inline void raw_spin_lock(struct raw_spinlock *lock) {
    while (true) {
        if (raw_spin_trylock(lock))
            return;

        while (atomic_load_explicit(&lock->state, memory_order_relaxed) != 0)
            cpu_relax();
    }
}

static inline void raw_spin_unlock(struct raw_spinlock *lock) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
}

/* whether interrupts were enabled on entry */
static inline bool __warn_unused_result
raw_spin_lock_irq_disable(struct raw_spinlock *lock) {
    bool irqs_were_enabled = are_interrupts_enabled();
    disable_interrupts();
    raw_spin_lock(lock);
    return irqs_were_enabled;
}

static inline void raw_spin_unlock_irq_restore(struct raw_spinlock *lock,
                                               bool irqs_were_enabled) {
    raw_spin_unlock(lock);
    if (irqs_were_enabled)
        enable_interrupts();
}
