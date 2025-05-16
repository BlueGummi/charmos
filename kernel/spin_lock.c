#include <spin_lock.h>

void spin_lock(struct spinlock *lock) {
    while (atomic_flag_test_and_set(&lock->lock)) {
        asm volatile("pause");
    }
}

void spin_unlock(struct spinlock *lock) {
    atomic_flag_clear(&lock->lock);
}
