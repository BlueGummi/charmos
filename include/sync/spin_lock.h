#pragma once
#include <asm.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define SPINLOCK_INIT {0}
struct spinlock {
    atomic_flag lock;
};

static inline void spinlock_init(struct spinlock *lock) {
    memset(lock, 0, sizeof(struct spinlock));
}

static inline bool spin_lock(struct spinlock *lock) {
    bool int_enabled = are_interrupts_enabled();
    clear_interrupts();
    
    while (atomic_flag_test_and_set(&lock->lock))
        cpu_relax();

    return int_enabled;
}

static inline void spin_unlock(struct spinlock *lock, bool interrupts_changed) {
    atomic_flag_clear(&lock->lock);

    if (interrupts_changed)
        restore_interrupts();
}

static inline void spin_lock_no_cli(struct spinlock *lock) {
    while (atomic_flag_test_and_set(&lock->lock))
        cpu_relax();
}

static inline void spin_unlock_no_cli(struct spinlock *lock) {
    atomic_flag_clear(&lock->lock);
}

static inline bool spin_trylock(struct spinlock *lock) {
    return !atomic_flag_test_and_set(&lock->lock);
}
