#pragma once
#include <asm.h>
#include <stdatomic.h>
#include <stdbool.h>

struct spinlock {
    atomic_uint state;
};

#define SPINLOCK_INIT {ATOMIC_VAR_INIT(0)}

static inline void spinlock_init(struct spinlock *lock) {
    atomic_store(&lock->state, 0);
}

static inline bool spin_lock(struct spinlock *lock) {
    bool int_enabled = are_interrupts_enabled();
    clear_interrupts();

    uint32_t expected;
    do {
        expected = 0;
        while (atomic_load_explicit(&lock->state, memory_order_relaxed) != 0)
            cpu_relax();
    } while (!atomic_compare_exchange_weak_explicit(&lock->state, &expected, 1,
                                                    memory_order_acquire,
                                                    memory_order_relaxed));

    return int_enabled;
}

static inline void spin_unlock(struct spinlock *lock, bool restore) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
    if (restore)
        restore_interrupts();
}

static inline void spin_lock_no_cli(struct spinlock *lock) {
    uint32_t expected;
    do {
        expected = 0;
        while (atomic_load_explicit(&lock->state, memory_order_relaxed) != 0)
            cpu_relax();
    } while (!atomic_compare_exchange_weak_explicit(&lock->state, &expected, 1,
                                                    memory_order_acquire,
                                                    memory_order_relaxed));
}

static inline void spin_unlock_no_cli(struct spinlock *lock) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
}

static inline bool spin_trylock(struct spinlock *lock) {
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        &lock->state, &expected, 1, memory_order_acquire, memory_order_relaxed);
}
