#include "sync/lock_chk/internal.h"
#include "sync/tests/test_internal.h"

#ifdef DEBUG_LOCK_CHK

#include <sync/lock_chk_assert.h>
#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>
#include <test/fleet.h>
#include <test/sync.h>
#include <time/spin_sleep.h>

LOCK_CHK_CLASS_DEFINE_LOCAL(assert_spin_class);
LOCK_CHK_CLASS_DEFINE_LOCAL(assert_qspin_class);
LOCK_CHK_CLASS_DEFINE_LOCAL(assert_mutex_class);
LOCK_CHK_CLASS_DEFINE_LOCAL(assert_mutex_simple_class);
LOCK_CHK_CLASS_DEFINE_LOCAL(assert_rwlock_class);
LOCK_CHK_CLASS_DEFINE_LOCAL(assert_cross_thread_class);

TEST_DEFINE_UNIT(lock_chk, assert_held_roundtrip_spin) {
    struct spinlock s;
    spinlock_init_chk(&s, LOCK_CHK_CLASS(assert_spin_class), LOCK_CHKD_FULL);

    SPINLOCK_ASSERT_NOT_HELD(&s);
    enum irql old = spin_lock(&s);
    SPINLOCK_ASSERT_HELD(&s);
    LOCK_CHK_ASSERT_HELD(&s);
    spin_unlock(&s, old);
    SPINLOCK_ASSERT_NOT_HELD(&s);
    LOCK_CHK_ASSERT_NOT_HELD(&s);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, assert_held_roundtrip_qspin) {
    struct qspinlock s;
    qspinlock_init_chk(&s, LOCK_CHK_CLASS(assert_qspin_class), LOCK_CHKD_FULL);

    QSPINLOCK_ASSERT_NOT_HELD(&s);
    enum irql old = qspin_lock(&s);
    QSPINLOCK_ASSERT_HELD(&s);
    qspin_unlock(&s, old);
    QSPINLOCK_ASSERT_NOT_HELD(&s);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, assert_held_roundtrip_mutex) {
    struct mutex m;
    mutex_init_chk(&m, LOCK_CHK_CLASS(assert_mutex_class), LOCK_CHKD_FULL);

    MUTEX_ASSERT_NOT_HELD(&m);
    mutex_lock(&m);
    MUTEX_ASSERT_HELD(&m);
    LOCK_CHK_ASSERT_HELD(&m);
    mutex_unlock(&m);
    MUTEX_ASSERT_NOT_HELD(&m);
    LOCK_CHK_ASSERT_NOT_HELD(&m);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, assert_held_roundtrip_mutex_simple) {
    struct mutex_simple m;
    mutex_simple_init_chk(&m, LOCK_CHK_CLASS(assert_mutex_simple_class),
                          LOCK_CHKD_FULL);

    MUTEX_SIMPLE_ASSERT_NOT_HELD(&m);
    mutex_simple_lock(&m);
    MUTEX_SIMPLE_ASSERT_HELD(&m);
    LOCK_CHK_ASSERT_HELD(&m);
    mutex_simple_unlock(&m);
    MUTEX_SIMPLE_ASSERT_NOT_HELD(&m);
    LOCK_CHK_ASSERT_NOT_HELD(&m);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, assert_held_roundtrip_rwlock) {
    struct rwlock rw;
    rwlock_init_chk(&rw, THREAD_PRIO_CLASS_TIMESHARE,
                    LOCK_CHK_CLASS(assert_rwlock_class), LOCK_CHKD_FULL);

    RWLOCK_ASSERT_NOT_HELD(&rw);

    rw_read_lock(&rw);
    RWLOCK_ASSERT_READ(&rw);
    LOCK_CHK_ASSERT_HELD(&rw, RWLOCK_READ);
    rw_unlock(&rw);
    RWLOCK_ASSERT_NOT_HELD(&rw);

    rw_write_lock(&rw);
    RWLOCK_ASSERT_WRITE(&rw);
    LOCK_CHK_ASSERT_HELD(&rw, RWLOCK_WRITE);
    rw_unlock(&rw);
    RWLOCK_ASSERT_NOT_HELD(&rw);
    LOCK_CHK_ASSERT_NOT_HELD(&rw);

    return TEST_SUCCESS;
}

struct cross_thread_fix {
    struct mutex lock;
    struct test_latch held;
    struct test_latch release;
};

static bool assert_cross_thread_worker(struct test_fleet *f,
                                       struct test_conc_worker *w) {
    struct cross_thread_fix *fix = w->arg;

    mutex_lock(&fix->lock);
    test_latch_set(&fix->held);
    TEST_WORKER_CHECK_GOTO(f, test_latch_wait_timeout(&fix->release, 5000),
                           out);
    mutex_unlock(&fix->lock);
    return true;

out:
    mutex_unlock(&fix->lock);
    return false;
}

TEST_DEFINE_UNIT(lock_chk, assert_not_held_cross_thread_mutex) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct cross_thread_fix *fix = test_new(fix);

    mutex_init_chk(&fix->lock, LOCK_CHK_CLASS(assert_cross_thread_class),
                   LOCK_CHKD_FULL);
    test_latch_init(&fix->held);
    test_latch_init(&fix->release);

    test_fleet_spawn(fleet, "cross_thread", assert_cross_thread_worker,
                     .arg = fix);
    test_fleet_start_all(fleet);

    /* Every assert below can return early */
    TEST_ASSERT(test_latch_wait_timeout(&fix->held, 5000));

    /* Worker holds this right now */
    TEST_ASSERT_TRUE(mutex_locked(&fix->lock));
    MUTEX_ASSERT_NOT_HELD(&fix->lock);

    test_latch_set(&fix->release);

    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    MUTEX_ASSERT_NOT_HELD(&fix->lock);
    return TEST_SUCCESS;
}

#endif /* DEBUG_LOCK_CHK */
