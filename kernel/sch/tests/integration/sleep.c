#include "../test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(sched, .intensity_desc = {
                              .curve = TEST_SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

static void sleepy_entry(void *) {
    thread_sleep_for_ms(50);
}

TEST_DECLARE_INTEGRATION(sched_sleepy_test, .group = TEST_GROUP(sched)) {
    struct thread *t =
        thread_spawn_joinable("sched_sleepy_test", sleepy_entry, NULL);
    TEST_ASSERT(t);
    thread_join(t);
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
    apc_init(apc, apc_si, NULL);

    while (!atomic_load(&si_started))
        cpu_relax();

    apc_enqueue(si_t, apc, APC_TYPE_KERNEL);
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

TEST_DECLARE_INTEGRATION(thread_sleep_interruptible_test,
                         .group = TEST_GROUP(sched)) {
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

    TEST_ASSERT(si_t && waker && enq);

    thread_join(si_t);
    thread_join(waker);
    thread_join(enq);

    TEST_ASSERT(atomic_load(&si_ok));

    return TEST_SUCCESS;
}
#endif
