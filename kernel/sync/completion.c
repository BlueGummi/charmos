#include <sch/sched.h>
#include <sync/completion.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>
#include <thread/thread_types.h>

#define COMPLETION_ALL (UINT32_MAX / 2)

LOCK_CHK_CLASS_DEFINE_LOCAL(completion_irq);
LOCK_CHK_CLASS_DEFINE_LOCAL(completion_disp);

void completion_init(struct completion *c, bool irq_disable) {
    atomic_init(&c->done, 0);
    c->irq_disable = irq_disable;
    if (irq_disable) {
        spinlock_init_chk(&c->lock, LOCK_CHK_CLASS(completion_irq),
                          LOCK_CHKD_FULL);
    } else {
        spinlock_init_chk(&c->lock, LOCK_CHK_CLASS(completion_disp),
                          LOCK_CHKD_FULL);
    }
    condvar_init(&c->cv);
}

static enum irql
completion_lock_internal(struct completion *c) TSA_NO_ANALYSIS {
    if (c->irq_disable)
        return spin_lock_high(&c->lock);

    return spin_lock(&c->lock);
}

void completion_reinit(struct completion *c) TSA_NO_ANALYSIS {
    enum irql irql = completion_lock_internal(c);
    atomic_store_rlx(&c->done, 0);
    spin_unlock(&c->lock, irql);
}

void complete(struct completion *c) TSA_NO_ANALYSIS {
    enum irql irql = completion_lock_internal(c);

    if (atomic_load_rlx(&c->done) < UINT32_MAX)
        atomic_inc_rlx(&c->done);

    condvar_signal(&c->cv);

    spin_unlock(&c->lock, irql);
}

void complete_all(struct completion *c) TSA_NO_ANALYSIS {
    enum irql irql = completion_lock_internal(c);

    atomic_store_rlx(&c->done, COMPLETION_ALL);

    condvar_broadcast(&c->cv);

    spin_unlock(&c->lock, irql);
}

void completion_wait(struct completion *c) TSA_NO_ANALYSIS {
    enum irql irql = completion_lock_internal(c);

    while (atomic_load_rlx(&c->done) == 0)
        condvar_wait(&c->cv, &c->lock, irql, &irql);

    if (atomic_load_rlx(&c->done) != COMPLETION_ALL)
        atomic_dec_rlx(&c->done);

    spin_unlock(&c->lock, irql);
}

bool completion_wait_timeout(struct completion *c,
                             time_ms_t timeout_ms) TSA_NO_ANALYSIS {
    enum irql irql = completion_lock_internal(c);

    while (atomic_load_rlx(&c->done) == 0) {
        enum wake_reason wr =
            condvar_wait_timeout(&c->cv, &c->lock, timeout_ms, irql, &irql);
        if (wr == WAKE_REASON_TIMEOUT && atomic_load_rlx(&c->done) == 0) {
            spin_unlock(&c->lock, irql);
            return false;
        }
    }

    if (atomic_load_rlx(&c->done) != COMPLETION_ALL)
        atomic_dec_rlx(&c->done);

    spin_unlock(&c->lock, irql);
    return true;
}

bool completion_try_wait(struct completion *c) TSA_NO_ANALYSIS {
    enum irql irql = completion_lock_internal(c);

    if (atomic_load_rlx(&c->done) == 0) {
        spin_unlock(&c->lock, irql);
        return false;
    }

    if (atomic_load_rlx(&c->done) != COMPLETION_ALL)
        atomic_dec_rlx(&c->done);

    spin_unlock(&c->lock, irql);
    return true;
}

bool completion_done(struct completion *c) {
    return atomic_load_rlx(&c->done) > 0;
}
