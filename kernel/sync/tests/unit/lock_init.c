#include "../test_internal.h"

#include <sync/seqlock.h>

#ifdef TEST_QSPINLOCK

LOCK_CHK_CLASS_DECLARE_LOCAL(lock_reinit_class);
static MUTEX_SIMPLE_DEFINE(static_simple_mutex);

#ifdef DEBUG_LOCK_CHK

static bool lock_init_state_valid(struct spinlock *spin,
                                  struct qspinlock *qspin, struct mutex *mutex,
                                  struct mutex_simple *simple,
                                  struct rwlock *rw, struct seqlock *seq) {
    return spin->chk_initialized && spin->chk_flags == LOCK_CHKD_FULL &&
           spin->chk_map.class != NULL && qspin->chk_initialized &&
           qspin->chk_flags == LOCK_CHKD_FULL && qspin->chk_map.class != NULL &&
           mutex->chk_initialized && mutex->chk_flags == LOCK_CHKD_FULL &&
           mutex->chk_map.class != NULL && simple->chk_initialized &&
           simple->chk_flags == LOCK_CHKD_FULL &&
           simple->chk_map.class != NULL && rw->chk_initialized &&
           rw->chk_flags == LOCK_CHKD_FULL && rw->chk_map.class != NULL &&
           seq->lock.chk_initialized && seq->lock.chk_flags == LOCK_CHKD_FULL &&
           seq->lock.chk_map.class != NULL;
}

static bool lock_reinit_state_valid(struct spinlock *spin,
                                    struct qspinlock *qspin,
                                    struct mutex *mutex,
                                    struct mutex_simple *simple,
                                    struct rwlock *rw) {
    const struct lock_chk_class *class = LOCK_CHK_CLASS(lock_reinit_class);

    return spin->chk_flags == LOCK_CHKD_FULL && spin->chk_map.class == class &&
           qspin->chk_flags == LOCK_CHKD_FULL &&
           qspin->chk_map.class == class &&
           mutex->chk_flags == LOCK_CHKD_FULL &&
           mutex->chk_map.class == class &&
           simple->chk_flags == LOCK_CHKD_FULL &&
           simple->chk_map.class == class && rw->chk_flags == LOCK_CHKD_FULL &&
           rw->chk_map.class == class;
}

#else /* !defined(DEBUG_LOCK_CHK) */

static bool lock_init_state_valid(struct spinlock *spin,
                                  struct qspinlock *qspin, struct mutex *mutex,
                                  struct mutex_simple *simple,
                                  struct rwlock *rw, struct seqlock *seq) {
    unused(spin, qspin, mutex, simple, rw, seq);
    return true;
}

static bool lock_reinit_state_valid(struct spinlock *spin,
                                    struct qspinlock *qspin,
                                    struct mutex *mutex,
                                    struct mutex_simple *simple,
                                    struct rwlock *rw) {
    unused(spin, qspin, mutex, simple, rw);
    return true;
}

#endif /* DEBUG_LOCK_CHK */

TEST_DECLARE_UNIT(qspinlock, lock_initializers_install_policy) {
    struct spinlock spin;
    struct qspinlock qspin;
    struct mutex mutex;
    struct mutex_simple simple;
    struct rwlock rw;
    struct seqlock seq;

    spinlock_init(&spin);
    qspinlock_init(&qspin);
    mutex_init(&mutex);
    mutex_simple_init(&simple);
    rwlock_init(&rw, THREAD_PRIO_CLASS_TIMESHARE);
    seqlock_init(&seq);

    TEST_ASSERT(list_empty(&static_simple_mutex.waiters.list));
    TEST_ASSERT(
        lock_init_state_valid(&spin, &qspin, &mutex, &simple, &rw, &seq));
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(qspinlock, lock_policy_mutation_and_reinit) {
    struct spinlock spin;
    struct qspinlock qspin;
    struct mutex mutex;
    struct mutex_simple simple;
    struct rwlock rw;
    const struct lock_chk_class *class = LOCK_CHK_CLASS(lock_reinit_class);

    spinlock_init(&spin);
    qspinlock_init(&qspin);
    mutex_init(&mutex);
    mutex_simple_init(&simple);
    rwlock_init(&rw, THREAD_PRIO_CLASS_TIMESHARE);

    spinlock_set_chk_flags(&spin, LOCK_CHKD_ORDER);
    qspinlock_set_chk_flags(&qspin, LOCK_CHKD_ORDER);
    mutex_set_chk_flags(&mutex, LOCK_CHKD_THREAD);
    mutex_simple_set_chk_flags(&simple, LOCK_CHKD_THREAD);
    rwlock_set_chk_flags(&rw, LOCK_CHKD_THREAD);

    spinlock_reinit_chk(&spin, class, LOCK_CHKD_FULL);
    qspinlock_reinit_chk(&qspin, class, LOCK_CHKD_FULL);
    mutex_reinit_chk(&mutex, class, LOCK_CHKD_FULL);
    mutex_simple_reinit_chk(&simple, class, LOCK_CHKD_FULL);
    rwlock_reinit_chk(&rw, THREAD_PRIO_CLASS_URGENT, class, LOCK_CHKD_FULL);

    TEST_ASSERT(lock_reinit_state_valid(&spin, &qspin, &mutex, &simple, &rw));
    return TEST_SUCCESS;
}

#endif /* TEST_QSPINLOCK */
