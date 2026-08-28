#include "../test_internal.h"
#include <asm.h>
#include <sync/lock_chk.h>
#include <sync/mutex.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>
#include <test/test.h>
#include <thread/thread.h>
#include <time/spin_sleep.h>

#ifdef DEBUG_LOCK_CHK
#include "../../lock_chk_internal.h"
#endif /* DEBUG_LOCK_CHK */

LOCK_CHK_CLASS_DECLARE_LOCAL(death_abba_class1);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_abba_class2);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_recurse_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_foreign_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_unbalanced_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_spin_class1);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_spin_class2);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_nmi_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_sleep_spin_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_exit_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_cross_rw_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_upgrade_rw_class);
LOCK_CHK_CLASS_DECLARE_LOCAL(death_exhaust_class);

/* ABBA cycle between two threads */
static struct mutex death_abba_m1;
static struct mutex death_abba_m2;
static _Atomic bool death_abba_ready = false;

static void death_abba_worker(void *arg) {
    unused(arg);
    mutex_lock(&death_abba_m2);
    atomic_store_explicit(&death_abba_ready, true, memory_order_release);
    sleep_spin_ms(10);
    mutex_lock(&death_abba_m1);
    mutex_unlock(&death_abba_m1);
    mutex_unlock(&death_abba_m2);
}

TEST_DECLARE_UNIT(qspinlock, lock_chk_death_abba_mutex,
                  .enabled = TEST_STATE_DISABLED) {
    mutex_init_chk(&death_abba_m1, LOCK_CHK_CLASS(death_abba_class1),
                   LOCK_CHKD_FULL);
    mutex_init_chk(&death_abba_m2, LOCK_CHK_CLASS(death_abba_class2),
                   LOCK_CHKD_FULL);
    thread_spawn("death_abba_worker", death_abba_worker, NULL);
    mutex_lock(&death_abba_m1);
    while (!atomic_load_explicit(&death_abba_ready, memory_order_acquire))
        sleep_spin_ms(1);
    mutex_lock(&death_abba_m2);
    mutex_unlock(&death_abba_m2);
    mutex_unlock(&death_abba_m1);
    return TEST_SUCCESS;
}

/* Recursive acquire */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_recursive_mutex,
                  .enabled = TEST_STATE_DISABLED) {
    struct mutex m;
    mutex_init_chk(&m, LOCK_CHK_CLASS(death_recurse_class), LOCK_CHKD_FULL);
    mutex_lock(&m);
    mutex_lock(&m);
    mutex_unlock(&m);
    return TEST_SUCCESS;
}

/* Foreign unlock */
static struct mutex death_foreign_m;

static void death_foreign_worker(void *arg) {
    unused(arg);
    mutex_unlock(&death_foreign_m);
}

TEST_DECLARE_UNIT(qspinlock, lock_chk_death_foreign_unlock,
                  .enabled = TEST_STATE_DISABLED) {
    mutex_init_chk(&death_foreign_m, LOCK_CHK_CLASS(death_foreign_class),
                   LOCK_CHKD_FULL);
    mutex_lock(&death_foreign_m);
    struct thread *th =
        thread_spawn_joinable("death_foreign", death_foreign_worker, NULL);
    thread_join(th);
    mutex_unlock(&death_foreign_m);
    return TEST_SUCCESS;
}

/* Unbalanced unlock */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_unbalanced_unlock,
                  .enabled = TEST_STATE_DISABLED) {
    struct mutex m;
    mutex_init_chk(&m, LOCK_CHK_CLASS(death_unbalanced_class), LOCK_CHKD_FULL);
    mutex_unlock(&m);
    return TEST_SUCCESS;
}

/* Non-LIFO spinlock unlock */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_spin_order,
                  .enabled = TEST_STATE_DISABLED) {
    struct spinlock s1, s2;
    spinlock_init_chk(&s1, LOCK_CHK_CLASS(death_spin_class1), LOCK_CHKD_FULL);
    spinlock_init_chk(&s2, LOCK_CHK_CLASS(death_spin_class2), LOCK_CHKD_FULL);
    enum irql i1 = spin_lock(&s1);
    enum irql i2 = spin_lock(&s2);
    spin_unlock(&s1, i1);
    spin_unlock(&s2, i2);
    return TEST_SUCCESS;
}

/* Inconsistent IRQ safety usage */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_irq_unsafe_spin,
                  .enabled = TEST_STATE_DISABLED) {
    struct spinlock s;
    spinlock_init_chk(&s, LOCK_CHK_CLASS(death_spin_class1), LOCK_CHKD_FULL);
    enum irql old = spin_lock(&s);
    spin_unlock(&s, old);
    old = spin_lock_irq_disable(&s);
    spin_unlock(&s, old);
    return TEST_SUCCESS;
}

/* lock acquire in NMI */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_checked_in_nmi,
                  .enabled = TEST_STATE_DISABLED) {
#ifdef DEBUG_LOCK_CHK
    struct lock_chk_map map =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(death_nmi_class));
    struct lock_chk_acquire_token tok;
    struct lock_chk_acquire_request req = {
        .map = &map,
        .instance = &map,
        .flags = LOCK_CHKD_FULL,
        .type = LOCK_CHK_TYPE_SPIN,
        .mode = LOCK_CHK_MODE_EXCLUSIVE,
        .wait_kind = LOCK_CHK_WAIT_BLOCKING,
        .in_nmi = true,
        .site = LOCK_CHK_SITE_HERE(),
    };
    lock_chk_before_acquire(&tok, &req);
#endif
    return TEST_SUCCESS;
}

/* Assert schedulable / sleep while holding a thread checked spinlock */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_sleep_holding_spin,
                  .enabled = TEST_STATE_DISABLED) {
    struct spinlock s;
    spinlock_init_chk(&s, LOCK_CHK_CLASS(death_sleep_spin_class),
                      LOCK_CHKD_FULL);
    enum irql old = spin_lock(&s);
    lock_chk_assert_schedulable(LOCK_CHK_SITE_HERE());
    spin_unlock(&s, old);
    return TEST_SUCCESS;
}

/* Thread exit while holding a thread checked lock */
static void death_exit_worker(void *arg) {
    unused(arg);
    static struct mutex exit_m;
    mutex_init_chk(&exit_m, LOCK_CHK_CLASS(death_exit_class), LOCK_CHKD_FULL);
    mutex_lock(&exit_m);
    /* Worker function returns without unlocking */
}

TEST_DECLARE_UNIT(qspinlock, lock_chk_death_exit_holding_lock,
                  .enabled = TEST_STATE_DISABLED) {
    struct thread *th =
        thread_spawn_joinable("death_exit_worker", death_exit_worker, NULL);
    thread_join(th);
    return TEST_SUCCESS;
}

/* Cross thread release on checked reader lock */
static struct rwlock death_cross_rw;

static void death_cross_rw_worker(void *arg) {
    unused(arg);
    rw_unlock(&death_cross_rw);
}

TEST_DECLARE_UNIT(qspinlock, lock_chk_death_rw_cross_thread_release,
                  .enabled = TEST_STATE_DISABLED) {
    rwlock_init_chk(&death_cross_rw, THREAD_PRIO_CLASS_TIMESHARE,
                    LOCK_CHK_CLASS(death_cross_rw_class), LOCK_CHKD_FULL);
    rw_read_lock(&death_cross_rw);
    struct thread *th =
        thread_spawn_joinable("death_cross_rw", death_cross_rw_worker, NULL);
    thread_join(th);
    rw_unlock(&death_cross_rw);
    return TEST_SUCCESS;
}

/* RW lock invalid upgrade / recursive acquire */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_rw_invalid_upgrade,
                  .enabled = TEST_STATE_DISABLED) {
    struct rwlock rw;
    rwlock_init_chk(&rw, THREAD_PRIO_CLASS_TIMESHARE,
                    LOCK_CHK_CLASS(death_upgrade_rw_class), LOCK_CHKD_FULL);
    rw_read_lock(&rw);
    rw_write_lock(&rw);
    rw_unlock(&rw);
    rw_unlock(&rw);
    return TEST_SUCCESS;
}

/* Uninitialized zero-filled lock usage */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_uninitialized,
                  .enabled = TEST_STATE_DISABLED) {
    struct mutex uninit_m = {0};
    mutex_lock(&uninit_m);
    return TEST_SUCCESS;
}

/* Exceed held capacity */
TEST_DECLARE_UNIT(qspinlock, lock_chk_death_exhaust_panic,
                  .enabled = TEST_STATE_DISABLED) {
    struct mutex m[33];
    for (int i = 0; i < 33; i++) {
        mutex_init_chk(&m[i], LOCK_CHK_CLASS(death_exhaust_class),
                       LOCK_CHKD_THREAD);
        mutex_lock(&m[i]);
    }
    for (int i = 32; i >= 0; i--)
        mutex_unlock(&m[i]);
    return TEST_SUCCESS;
}
