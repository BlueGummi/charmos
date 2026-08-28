#include "sch/tests/test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(sched, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

static void sleepy_entry(void *) {
    thread_sleep_for_ms(50);
}

TEST_DECLARE_INTEGRATION(sched, sleep_ms) {
    struct thread *t =
        thread_spawn_joinable("sched_sleepy_test", sleepy_entry, NULL);
    TEST_ASSERT_NONNULL(t);
    thread_join(t);
    return TEST_SUCCESS;
}

static atomic_bool slept_for_us = false;

static void micro_sleep_entry(void *arg) {
    (void) arg;
    thread_sleep_for_us(200);
    atomic_store(&slept_for_us, true);
}

TEST_DECLARE_INTEGRATION(sched, sleep_us) {
    atomic_store(&slept_for_us, false);
    struct thread *t = thread_spawn_joinable("sched_micro_sleep_test",
                                             micro_sleep_entry, NULL);
    TEST_ASSERT_NONNULL(t);
    thread_join(t);
    TEST_ASSERT(atomic_load(&slept_for_us));
    return TEST_SUCCESS;
}

static atomic_bool short_sleep_stop = false;
static atomic_size_t short_sleep_count = 0;

static void short_sleep_entry(void *arg) {
    (void) arg;
    for (size_t i = 0; i < 200 && !atomic_load(&short_sleep_stop); i++) {
        thread_sleep_for_us(1);
        atomic_fetch_add(&short_sleep_count, 1);
    }
}

TEST_DECLARE_INTEGRATION(sched, short_sleep_lost_wake) {
    atomic_store(&short_sleep_stop, false);
    atomic_store(&short_sleep_count, 0);

    struct thread *t = thread_spawn_joinable("sched_short_sleep_lost_wake_test",
                                             short_sleep_entry, NULL);
    TEST_ASSERT_NONNULL(t);

    bool joined = thread_join_timeout(t, 1000, NULL);
    if (!joined) {
        atomic_store(&short_sleep_stop, true);
        thread_wake(t, THREAD_WAKE_REASON_SLEEP_TIMEOUT,
                    t->perceived_prio_class, t);
        thread_join(t);
    }

    if (!joined)
        test_info("short timed sleep lost its wake after %zu cycles",
                  atomic_load(&short_sleep_count));
    TEST_ASSERT(joined);
    TEST_ASSERT_EQ(200, atomic_load(&short_sleep_count));
    return TEST_SUCCESS;
}

static atomic_bool si_apc_ran = false;
static struct thread *si_t;
static atomic_bool si_ok = false;
static atomic_bool si_started = false;

static void apc_si(void *apc) {
    (void) apc;
    atomic_store(&si_apc_ran, true);
}

static void apc_enqueue_thread(void *) {
    struct apc *apc = apc_create();
    apc_init(apc, apc_si, NULL, apc_destroy_free);

    while (!atomic_load(&si_started))
        cpu_relax();

    if (thread_get(si_t)) {
        apc_enqueue(si_t, apc, APC_TYPE_KERNEL);
        thread_put(si_t);
    }
    apc_put(apc);
}

static void sleeping_thread(void *) {
    atomic_store(&si_started, true);

    thread_prepare_to_sleep(thread_get_current(), THREAD_SLEEP_REASON_MANUAL,
                            THREAD_WAIT_INTERRUPTIBLE, (void *) 4);

    thread_yield_until_wake_match();

    atomic_store(&si_ok, true);
}

static void waking_thread(void *) {
    while (!atomic_load(&si_apc_ran))
        scheduler_yield();

    thread_wake(si_t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                si_t->perceived_prio_class, (void *) 4);
}

TEST_DECLARE_INTEGRATION(sched, sleep_interruptible_apc) {
    if (global.core_count < 4) {
        test_info("too few cores");
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    atomic_store(&si_apc_ran, false);
    atomic_store(&si_ok, false);
    atomic_store(&si_started, false);

    si_t = thread_spawn_joinable_on_core("si_thread", sleeping_thread, NULL, 1);
    struct thread *waker =
        thread_spawn_joinable_on_core("si_wake", waking_thread, NULL, 2);
    struct thread *enq =
        thread_spawn_joinable_on_core("si_apc_e", apc_enqueue_thread, NULL, 3);

    TEST_ASSERT_NONNULL(si_t);
    TEST_ASSERT_NONNULL(waker);
    TEST_ASSERT_NONNULL(enq);

    thread_join(si_t);
    thread_join(waker);
    thread_join(enq);

    TEST_ASSERT(atomic_load(&si_ok));

    return TEST_SUCCESS;
}
#endif
