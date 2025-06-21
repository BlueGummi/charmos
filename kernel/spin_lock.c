#include <asm.h>
#include <spin_lock.h>

bool spin_lock(struct spinlock *lock) {
    bool int_enabled = are_interrupts_enabled();
    if (int_enabled) {
        asm volatile("cli");
    }

    while (atomic_flag_test_and_set(&lock->lock)) {
        asm volatile("pause");
    }
    return int_enabled;
}

void spin_unlock(struct spinlock *lock, bool interrupts_changed) {
    atomic_flag_clear(&lock->lock);
    if (interrupts_changed) {
        asm volatile("sti");
    }
}

void spin_lock_no_cli(struct spinlock *lock) {
    while (atomic_flag_test_and_set(&lock->lock)) {
        asm volatile("pause");
    }
}

void spin_unlock_no_cli(struct spinlock *lock) {
    atomic_flag_clear(&lock->lock);
}

bool spin_trylock(struct spinlock *lock) {
    return !atomic_flag_test_and_set(&lock->lock);
}

