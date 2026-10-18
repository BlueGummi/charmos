#include "sch/tests/test_internal.h"

TEST_GROUP_DEFINE(sched, .intensity_desc = {
                             .curve = SCALE_PIECEWISE_LOG,
                             .unit = "iters",
                         });

static void sleepy_entry(void *arg) {
    cc_ignore(arg);
    thread_sleep_for_ms(50);
}

TEST_DEFINE_INTEGRATION(sched, sleep_ms) {
    struct thread *t =
        thread_spawn("sched_sleepy_test", sleepy_entry, .joinable = true);
    TEST_ASSERT_NONNULL(t);
    thread_join(t);
    return TEST_SUCCESS;
}

static atomic_bool slept_for_us = false;

static void micro_sleep_entry(void *arg) {
    cc_ignore(arg);
    thread_sleep_for_us(200);
    atomic_store(&slept_for_us, true);
}

TEST_DEFINE_INTEGRATION(sched, sleep_us) {
    atomic_store(&slept_for_us, false);
    struct thread *t = thread_spawn("sched_micro_sleep_test", micro_sleep_entry,
                                    .joinable = true);
    TEST_ASSERT_NONNULL(t);
    thread_join(t);
    TEST_ASSERT(atomic_load(&slept_for_us));
    return TEST_SUCCESS;
}

static atomic_bool short_sleep_stop = false;
static atomic_size_t short_sleep_count = 0;

static void short_sleep_entry(void *arg) {
    cc_ignore(arg);
    for (size_t i = 0; i < 200 && !atomic_load(&short_sleep_stop); i++) {
        thread_sleep_for_us(1);
        atomic_inc(&short_sleep_count);
    }
}

TEST_DEFINE_INTEGRATION(sched, short_sleep_lost_wake) {
    atomic_store(&short_sleep_stop, false);
    atomic_store(&short_sleep_count, 0);

    struct thread *t = thread_spawn("sched_short_sleep_lost_wake_test",
                                    short_sleep_entry, .joinable = true);
    TEST_ASSERT_NONNULL(t);

    bool joined = thread_join_timeout(t, 1000, NULL);
    if (!joined) {
        atomic_store(&short_sleep_stop, true);
        /* alert must not rescue a uninterruptible wait */
        enforce(joined, "short timed sleep lost its object completion");
        thread_join(t);
    }

    if (!joined)
        test_info("short timed sleep lost its wake after %zu cycles",
                  atomic_load(&short_sleep_count));
    TEST_ASSERT(joined);
    TEST_ASSERT_EQ(200, atomic_load(&short_sleep_count));
    return TEST_SUCCESS;
}

static struct thread_wait_header si_wait;
static atomic_bool si_apc_ran = false;
static struct thread *si_t;
static atomic_bool si_ok = false;
static atomic_bool si_started = false;

static void apc_si(struct apc_context *ac, void *apc) {
    cc_ignore(ac);
    cc_ignore(apc);
    atomic_store(&si_apc_ran, true);
}

static void apc_enqueue_thread(void *arg) {
    cc_ignore(arg);
    struct apc *apc = apc_create();
    apc_init(apc, apc_si, NULL, apc_destroy_free);

    while (!atomic_load(&si_started))
        cpu_pause();

    rcu_read_lock();
    bool got = thread_getref_rcu(si_t);
    rcu_read_unlock();

    if (got) {
        apc_enqueue(si_t, apc, APC_TYPE_KERNEL);
        thread_putref(si_t);
    }
    apc_putref(apc);
}

static void sleeping_thread(void *arg) {
    cc_ignore(arg);
    thread_wait_prepare_to_sleep(&si_wait, &si_wait, THREAD_WAIT_INTERRUPTIBLE);
    atomic_store(&si_started, true);
    thread_wait_complete();

    atomic_store(&si_ok, true);
}

static void waking_thread(void *arg) {
    cc_ignore(arg);
    while (!atomic_load(&si_apc_ran))
        scheduler_yield();

    thread_wait_header_satisfy(&si_wait, THREAD_WAKE_REASON_SLEEP_MANUAL, NULL);
}

TEST_DEFINE_INTEGRATION(sched, sleep_interruptible_apc, .min_cores = 4) {
    thread_wait_header_init(&si_wait);
    atomic_store(&si_apc_ran, false);
    atomic_store(&si_ok, false);
    atomic_store(&si_started, false);

    si_t = thread_spawn("si_thread", sleeping_thread, .on_cpu = 1,
                        .joinable = true);
    struct thread *waker =
        thread_spawn("si_wake", waking_thread, .on_cpu = 2, .joinable = true);
    struct thread *enq = thread_spawn("si_apc_e", apc_enqueue_thread,
                                      .on_cpu = 3, .joinable = true);

    TEST_ASSERT_NONNULL(si_t);
    TEST_ASSERT_NONNULL(waker);
    TEST_ASSERT_NONNULL(enq);

    thread_join(si_t);
    thread_join(waker);
    thread_join(enq);

    TEST_ASSERT(atomic_load(&si_ok));

    return TEST_SUCCESS;
}

static atomic_bool sub_apc_ran = false;
static struct thread *sub_t;
static atomic_bool sub_interrupted = false;
static atomic_bool sub_started = false;

static void apc_sub(struct apc_context *ac, void *apc) {
    cc_ignore(ac);
    cc_ignore(apc);
    atomic_store(&sub_apc_ran, true);
}

static void apc_sub_enq_thread(void *arg) {
    cc_ignore(arg);
    struct apc *apc = apc_create();
    apc_init(apc, apc_sub, NULL, apc_destroy_free);

    while (!atomic_load(&sub_started))
        cpu_pause();

    rcu_read_lock();
    bool got = thread_getref_rcu(sub_t);
    rcu_read_unlock();

    if (got) {
        apc_enqueue(sub_t, apc, APC_TYPE_KERNEL);
        while (!atomic_load(&sub_apc_ran))
            scheduler_yield();
        thread_alert(sub_t);
        thread_putref(sub_t);
    }
    apc_putref(apc);
}

static void sleeping_sub_thread(void *arg) {
    cc_ignore(arg);
    atomic_store(&sub_started, true);

    thread_park();
    atomic_store(&sub_interrupted, true);
}

TEST_DEFINE_INTEGRATION(sched, wait_interruptible_substrate, .min_cores = 3) {
    atomic_store(&sub_apc_ran, false);
    atomic_store(&sub_interrupted, false);
    atomic_store(&sub_started, false);

    sub_t = thread_spawn("sub_th", sleeping_sub_thread, .on_cpu = 1,
                         .joinable = true);
    struct thread *enq = thread_spawn("sub_enq", apc_sub_enq_thread,
                                      .on_cpu = 2, .joinable = true);

    TEST_ASSERT_NONNULL(sub_t);
    TEST_ASSERT_NONNULL(enq);

    thread_join(sub_t);
    thread_join(enq);

    TEST_ASSERT(atomic_load(&sub_apc_ran));
    TEST_ASSERT(atomic_load(&sub_interrupted));

    return TEST_SUCCESS;
}

static struct thread *arb_t;
static atomic_bool arb_started = false;
static atomic_bool arb_matched = false;

static void arbitrary_sleeping_thread(void *arg) {
    cc_ignore(arg);
    atomic_store(&arb_started, true);

    thread_park();
    atomic_store(&arb_matched, true);
}

static void arbitrary_waking_thread(void *arg) {
    cc_ignore(arg);
    while (!atomic_load(&arb_started))
        cpu_pause();

    thread_sleep_for_ms(5);

    thread_alert(arb_t);
}

TEST_DEFINE_INTEGRATION(sched, park_alert, .min_cores = 3) {
    atomic_store(&arb_started, false);
    atomic_store(&arb_matched, false);

    arb_t = thread_spawn("arb_th", arbitrary_sleeping_thread, .on_cpu = 1,
                         .joinable = true);
    struct thread *waker = thread_spawn("arb_waker", arbitrary_waking_thread,
                                        .on_cpu = 2, .joinable = true);

    TEST_ASSERT_NONNULL(arb_t);
    TEST_ASSERT_NONNULL(waker);

    thread_join(arb_t);
    thread_join(waker);

    TEST_ASSERT(atomic_load(&arb_matched));

    return TEST_SUCCESS;
}
