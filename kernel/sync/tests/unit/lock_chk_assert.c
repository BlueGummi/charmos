#include "sync/tests/test_internal.h"

#ifdef DEBUG_LOCK_CHK

#include <sync/lock_chk_assert.h>
#include <sync/lock_chk_internal.h>
#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>
#include <time/spin_sleep.h>

LOCK_CHK_CLASS_DECLARE_LOCAL(assert_spin_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(assert_qspin_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(assert_mutex_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(assert_mutex_simple_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(assert_rwlock_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(assert_cross_thread_class);

TEST_DECLARE_UNIT(lock_chk, assert_held_roundtrip_spin) {
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

TEST_DECLARE_UNIT(lock_chk, assert_held_roundtrip_qspin) {
    struct qspinlock s;
    qspinlock_init_chk(&s, LOCK_CHK_CLASS(assert_qspin_class), LOCK_CHKD_FULL);

    QSPINLOCK_ASSERT_NOT_HELD(&s);
    enum irql old = qspin_lock(&s);
    QSPINLOCK_ASSERT_HELD(&s);
    qspin_unlock(&s, old);
    QSPINLOCK_ASSERT_NOT_HELD(&s);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk, assert_held_roundtrip_mutex) {
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

TEST_DECLARE_UNIT(lock_chk, assert_held_roundtrip_mutex_simple) {
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

TEST_DECLARE_UNIT(lock_chk, assert_held_roundtrip_rwlock) {
    struct rwlock rw;
    rwlock_init_chk(&rw, THREAD_PRIO_CLASS_TIMESHARE,
                    LOCK_CHK_CLASS(assert_rwlock_class), LOCK_CHKD_FULL);

    RWLOCK_ASSERT_NOT_HELD(&rw);

    rw_read_lock(&rw);
    RWLOCK_ASSERT_READ(&rw);
    LOCK_CHK_ASSERT_HELD(&rw, RWLOCK_ACQUIRE_READ);
    rw_unlock(&rw);
    RWLOCK_ASSERT_NOT_HELD(&rw);

    rw_write_lock(&rw);
    RWLOCK_ASSERT_WRITE(&rw);
    LOCK_CHK_ASSERT_HELD(&rw, RWLOCK_ACQUIRE_WRITE);
    rw_unlock(&rw);
    RWLOCK_ASSERT_NOT_HELD(&rw);
    LOCK_CHK_ASSERT_NOT_HELD(&rw);

    return TEST_SUCCESS;
}

static struct mutex assert_cross_thread_mutex;
static _Atomic bool assert_cross_thread_held = false;
static _Atomic bool assert_cross_thread_release = false;

static void assert_cross_thread_worker(void *arg) {
    unused(arg);
    mutex_lock(&assert_cross_thread_mutex);
    atomic_store_explicit(&assert_cross_thread_held, true,
                          memory_order_release);
    while (!atomic_load_explicit(&assert_cross_thread_release,
                                 memory_order_acquire))
        sleep_spin_ms(1);
    mutex_unlock(&assert_cross_thread_mutex);
}

TEST_DECLARE_UNIT(lock_chk, assert_not_held_cross_thread_mutex) {
    mutex_init_chk(&assert_cross_thread_mutex,
                   LOCK_CHK_CLASS(assert_cross_thread_class), LOCK_CHKD_FULL);

    struct thread *th = thread_spawn_joinable("assert_cross_thread_worker",
                                              assert_cross_thread_worker, NULL);

    while (
        !atomic_load_explicit(&assert_cross_thread_held, memory_order_acquire))
        sleep_spin_ms(1);

    /* Worker holds this right now */
    TEST_ASSERT_TRUE(mutex_locked(&assert_cross_thread_mutex));
    MUTEX_ASSERT_NOT_HELD(&assert_cross_thread_mutex);

    atomic_store_explicit(&assert_cross_thread_release, true,
                          memory_order_release);
    thread_join(th);

    MUTEX_ASSERT_NOT_HELD(&assert_cross_thread_mutex);

    return TEST_SUCCESS;
}

#endif /* DEBUG_LOCK_CHK */
