#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/thread_types.h>

LOCK_CHK_CLASS_DECLARE_LOCAL(semaphore_irq);
LOCK_CHK_CLASS_DECLARE_LOCAL(semaphore_disp);

void semaphore_init(struct semaphore *s, int value, bool irq_disable) {
    s->count = value;
    s->irq_disable = irq_disable;
    if (irq_disable) {
        spinlock_init_chk(&s->lock, LOCK_CHK_CLASS(semaphore_irq),
                          LOCK_CHKD_FULL);
    } else {
        spinlock_init_chk(&s->lock, LOCK_CHK_CLASS(semaphore_disp),
                          LOCK_CHKD_FULL);
    }
    condvar_init(&s->cv, irq_disable);
}

static enum irql semaphore_lock_internal(struct semaphore *sem) {
    if (sem->irq_disable)
        return spin_lock_irq_disable(&sem->lock);

    return spin_lock(&sem->lock);
}

void semaphore_wait(struct semaphore *s) {
    enum irql irql = semaphore_lock_internal(s);

    while (atomic_load(&s->count) == 0)
        condvar_wait(&s->cv, &s->lock, irql, &irql);

    atomic_fetch_sub(&s->count, 1);
    spin_unlock(&s->lock, irql);
}

bool semaphore_timedwait(struct semaphore *s, time_ms_t timeout_ms) {
    enum irql irql = semaphore_lock_internal(s);

    while (atomic_load(&s->count) == 0) {
        enum wake_reason wr =
            condvar_wait_timeout(&s->cv, &s->lock, timeout_ms, irql, &irql);
        if (wr == WAKE_REASON_TIMEOUT && atomic_load(&s->count) == 0) {
            spin_unlock(&s->lock, irql);
            return false;
        }
    }

    atomic_fetch_sub(&s->count, 1);
    spin_unlock(&s->lock, irql);

    return true;
}

void semaphore_post(struct semaphore *s) {
    enum irql irql = semaphore_lock_internal(s);

    atomic_fetch_add(&s->count, 1);

    condvar_signal(&s->cv);

    spin_unlock(&s->lock, irql);
}

void semaphore_postn(struct semaphore *s, int n) {
    enum irql irql = semaphore_lock_internal(s);

    atomic_fetch_add(&s->count, n);
    for (int i = 0; i < n; i++)
        condvar_signal(&s->cv);

    spin_unlock(&s->lock, irql);
}
