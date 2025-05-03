#include <slock.h>

void spin_lock(struct spinlock *lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        asm volatile("pause");
    }
}

void spin_unlock(struct spinlock *lock) {
    __sync_lock_release(&lock->lock);
}
