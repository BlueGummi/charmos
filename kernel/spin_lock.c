#include <asm.h>
#include <spin_lock.h>

// TODO: This terrible implementation won't work across many cores :c

void spin_lock(struct spinlock *lock) {
    bool int_enabled = are_interrupts_enabled();
    if (int_enabled) {
        asm volatile("cli");
    }

    while (atomic_flag_test_and_set(&lock->lock)) {
        asm volatile("pause");
    }

    lock->interrupts_changed = int_enabled;
}

void spin_unlock(struct spinlock *lock) {
    atomic_flag_clear(&lock->lock);
    if (lock->interrupts_changed) {
        asm volatile("sti");
    }
}
