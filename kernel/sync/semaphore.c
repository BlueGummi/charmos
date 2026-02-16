#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/thread_types.h>

#define get_count(sem) atomic_load(&sem->count)
#define set_count(sem, val) atomic_store(&sem->count, val)
#define inc_count(sem) atomic_fetch_add(&sem->count, 1)
#define dec_count(sem) atomic_fetch_sub(&sem->count, 1)
#define add_count(sem, val) atomic_fetch_add(&sem->count, val)

void semaphore_init(struct semaphore *s, int value, bool irq_disable) {
    s->count = value;
    s->irq_disable = irq_disable;
    spinlock_init(&s->lock);
    condvar_init(&s->cv, irq_disable);
}

static enum irql semaphore_lock_internal(struct semaphore *sem) {
    if (sem->irq_disable)
        return spin_lock_irq_disable(&sem->lock);

    return spin_lock(&sem->lock);
}

void semaphore_wait(struct semaphore *s) {
    enum irql irql = semaphore_lock_internal(s);

    while (get_count(s) == 0)
        condvar_wait(&s->cv, &s->lock, irql, &irql);

    dec_count(s);
    semaphore_unlock(s, irql);
}

bool semaphore_timedwait(struct semaphore *s, time_t timeout_ms) {
    enum irql irql = semaphore_lock_internal(s);

    while (get_count(s) == 0) {
        enum irql out;
        if (!condvar_wait_timeout(&s->cv, &s->lock, timeout_ms, irql, &out)) {
            semaphore_unlock(s, out);
            return false;
        }
    }

    dec_count(s);
    semaphore_unlock(s, irql);

    return true;
}

void semaphore_post(struct semaphore *s) {
    enum irql irql = semaphore_lock_internal(s);

    inc_count(s);

    condvar_signal(&s->cv);

    semaphore_unlock(s, irql);
}

void semaphore_postn(struct semaphore *s, int n) {
    enum irql irql = semaphore_lock_internal(s);

    add_count(s, n);
    for (int i = 0; i < n; i++)
        condvar_signal(&s->cv);

    semaphore_unlock(s, irql);
}

void semaphore_post_callback(struct semaphore *s, thread_action_callback cb) {
    enum irql irql = semaphore_lock_internal(s);

    inc_count(s);

    condvar_signal_callback(&s->cv, cb);

    semaphore_unlock(s, irql);
}

void semaphore_postn_callback(struct semaphore *s, int n,
                              thread_action_callback cb) {
    enum irql irql = semaphore_lock_internal(s);

    add_count(s, n);
    for (int i = 0; i < n; i++)
        condvar_signal_callback(&s->cv, cb);

    semaphore_unlock(s, irql);
}
