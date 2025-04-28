#include <slock.h>

void spinlock_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        asm volatile("pause");
    }
}

void spinlock_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->lock);
}
