#include <sync/condvar.h>
#include <sync/semaphore.h>
#include <sync/spin_lock.h>

void semaphore_init(struct semaphore *s, int value) {
    s->count = value;
    spinlock_init(&s->lock);
    condvar_init(&s->cv);
}

void semaphore_wait(struct semaphore *s) {
    bool iflag = semaphore_lock(s);

    while (s->count == 0) {
        condvar_wait(&s->cv, &s->lock, false);
    }

    s->count--;

    semaphore_unlock(s, iflag);
}

bool semaphore_timedwait(struct semaphore *s, time_t timeout_ms) {
    bool iflag = semaphore_lock(s);

    while (s->count == 0) {
        if (!condvar_wait_timeout(&s->cv, &s->lock, timeout_ms, false)) {
            spin_unlock(&s->lock, iflag);
            return false;
        }
    }

    s->count--;

    semaphore_unlock(s, iflag);
    return true;
}

void semaphore_post(struct semaphore *s) {
    bool iflag = semaphore_lock(s);

    s->count++;
    condvar_signal(&s->cv);

    semaphore_unlock(s, iflag);
}

void semaphore_postn(struct semaphore *s, int n) {
    bool iflag = semaphore_lock(s);

    s->count += n;
    for (int i = 0; i < n; i++)
        condvar_signal(&s->cv);

    semaphore_unlock(s, iflag);
}
