#include <slock.h>

void spinlock_lock(struct spinlock *lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        asm volatile("pause");
    }
}

void spinlock_unlock(struct spinlock *lock) {
    __sync_lock_release(&lock->lock);
}
