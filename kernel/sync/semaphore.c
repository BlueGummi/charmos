#include <sync/condvar.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

#define get_count(sem) atomic_load(&sem->count)
#define set_count(sem, val) atomic_store(&sem->count, val)
#define inc_count(sem) atomic_fetch_add(&sem->count, 1)
#define dec_count(sem) atomic_fetch_sub(&sem->count, 1)
#define add_count(sem, val) atomic_fetch_add(&sem->count, val)

void semaphore_init(struct semaphore *s, int value) {
    s->count = value;
    spinlock_init(&s->lock);
    condvar_init(&s->cv);
}

void semaphore_wait(struct semaphore *s) {
    enum irql irql = semaphore_lock(s);

    while (s->count == 0)
        condvar_wait(&s->cv, &s->lock, false);

    dec_count(s);
    semaphore_unlock(s, irql);
}

bool semaphore_timedwait(struct semaphore *s, time_t timeout_ms) {
    enum irql irql = semaphore_lock(s);

    while (get_count(s) == 0) {
        if (!condvar_wait_timeout(&s->cv, &s->lock, timeout_ms, false)) {
            semaphore_unlock(s, irql);
            return false;
        }
    }

    dec_count(s);
    semaphore_unlock(s, irql);

    return true;
}

void semaphore_post(struct semaphore *s) {
    enum irql irql = semaphore_lock(s);

    inc_count(s);
    condvar_signal(&s->cv);

    semaphore_unlock(s, irql);
}

void semaphore_postn(struct semaphore *s, int n) {
    enum irql irql = semaphore_lock(s);

    add_count(s, n);
    for (int i = 0; i < n; i++)
        condvar_signal(&s->cv);

    semaphore_unlock(s, irql);
}
