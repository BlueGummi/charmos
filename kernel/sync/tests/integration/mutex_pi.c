#include "../test_internal.h"

#ifdef TEST_MUTEX


#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct mutex pi_mutex = MUTEX_INIT;
static struct thread *pi_ts, *pi_rt, *pi_dum;
static atomic_bool pi_ts_got = false;
static atomic_uint pi_done = 0;

static void pi_dummy(void *nothing) {
    (void) nothing;
    test_info("dummy");
    while (atomic_load(&pi_done) < 1)
        scheduler_yield();

    atomic_fetch_add(&pi_done, 1);
    test_info("exiting");
}

static void pi_rt_thread(void *nothing) {
    (void) nothing;
    mutex_lock(&pi_mutex);
    test_info("lock");
    kassert(mutex_get_owner(&pi_mutex) == thread_get_current());
    mutex_unlock(&pi_mutex);
    test_info("unlock");
    atomic_fetch_add(&pi_done, 1);
    test_info("exiting");
}

static void pi_ts_thread(void *nothing) {
    (void) nothing;
    mutex_lock(&pi_mutex);
    test_info("lock");
    atomic_store(&pi_ts_got, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    kassert(mutex_get_owner(&pi_mutex) == thread_get_current());
    test_info("boosted");

    test_info("unlock");
    mutex_unlock(&pi_mutex);

    atomic_fetch_add(&pi_done, 1);
    test_info("exiting");
}

TEST_DECLARE_INTEGRATION(mutex_pi_test, .group = TEST_GROUP(mutex)) {
    if (global.core_count == 1) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    atomic_store(&pi_ts_got, false);
    atomic_store(&pi_done, 0);

    cpu_id_t cpu = 1;
    pi_ts = thread_create("pi_ts", pi_ts_thread, NULL);
    pi_rt = thread_create("pi_rt", pi_rt_thread, NULL);
    pi_dum = thread_create("pi_dum", pi_dummy, NULL);
    pi_rt->perceived_prio_class = THREAD_PRIO_CLASS_RT;
    pi_dum->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_pin(pi_dum);
    thread_pin(pi_ts);
    thread_pin(pi_rt);

    thread_set_joinable(pi_ts);
    thread_set_joinable(pi_rt);
    thread_set_joinable(pi_dum);

    thread_enqueue_on_core(pi_ts, cpu);

    /* Wait for them to get the mutex, then RTs run */
    while (!atomic_load(&pi_ts_got))
        scheduler_yield();

    thread_enqueue_on_core(pi_dum, cpu);
    thread_enqueue_on_core(pi_rt, cpu);

    thread_join(pi_ts);
    thread_join(pi_rt);
    thread_join(pi_dum);

    TEST_ASSERT(atomic_load(&pi_done) == 3);

    return TEST_SUCCESS;
}

static struct mutex pi_mtx_a = MUTEX_INIT;
static struct mutex pi_mtx_b = MUTEX_INIT;

static struct thread *pi_ts1, *pi_ts2, *pi_rt2;
static atomic_uint pi_chain_done = 0;
static atomic_bool ts1_grabbed_a = false;
static atomic_bool ts2_grabbed_b = false;

static void pi_chain_ts2(void *arg) {
    (void) arg;
    mutex_lock(&pi_mtx_b);
    test_info("ts2 lock b");
    atomic_store(&ts2_grabbed_b, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    test_info("ts2 boosted");
    mutex_unlock(&pi_mtx_b);
    atomic_fetch_add(&pi_chain_done, 1);
}

static void pi_chain_ts1(void *arg) {
    (void) arg;
    mutex_lock(&pi_mtx_a);
    test_info("ts1 lock a");
    atomic_store(&ts1_grabbed_a, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    mutex_lock(&pi_mtx_b);
    test_info("ts1 lock b");

    mutex_unlock(&pi_mtx_b);
    mutex_unlock(&pi_mtx_a);
    atomic_fetch_add(&pi_chain_done, 1);
}

static void pi_chain_rt(void *arg) {
    (void) arg;
    test_info("rt lock");
    mutex_lock(&pi_mtx_a);
    test_info("rt lock got");

    mutex_unlock(&pi_mtx_a);
    atomic_fetch_add(&pi_chain_done, 1);
}

TEST_DECLARE_INTEGRATION(mutex_pi_chain, .group = TEST_GROUP(mutex)) {
    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    atomic_store(&pi_chain_done, 0);
    atomic_store(&ts1_grabbed_a, false);
    atomic_store(&ts2_grabbed_b, false);

    cpu_id_t cpu = 1;

    pi_ts2 = thread_create("pi_ts2", pi_chain_ts2, NULL);
    pi_ts1 = thread_create("pi_ts1", pi_chain_ts1, NULL);
    pi_rt2 = thread_create("pi_rt2", pi_chain_rt, NULL);

    pi_rt2->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_pin(pi_ts1);
    thread_pin(pi_ts2);
    thread_pin(pi_rt2);

    thread_set_joinable(pi_ts2);
    thread_set_joinable(pi_ts1);
    thread_set_joinable(pi_rt2);

    thread_enqueue_on_core(pi_ts2, cpu);
    while (!atomic_load(&ts2_grabbed_b))
        scheduler_yield();

    thread_enqueue_on_core(pi_ts1, cpu);

    while (!atomic_load(&ts1_grabbed_a))
        scheduler_yield();

    thread_enqueue_on_core(pi_rt2, cpu);

    thread_join(pi_ts2);
    thread_join(pi_ts1);
    thread_join(pi_rt2);

    TEST_ASSERT(atomic_load(&pi_chain_done) == 3);

    return TEST_SUCCESS;
}

static struct mutex pi_multi_mtx = MUTEX_INIT;
static atomic_uint pi_multi_done = 0;
static atomic_bool ts_got = false;

static void pi_multi_ts(void *arg) {
    (void) arg;
    mutex_lock(&pi_multi_mtx);
    test_info("multi_ts running");
    atomic_store(&ts_got, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    test_info("ts boosted");
    mutex_unlock(&pi_multi_mtx);
    atomic_fetch_add(&pi_multi_done, 1);
}

static void pi_multi_rt(void *arg) {
    (void) arg;
    test_info("multi_rt running");
    mutex_lock(&pi_multi_mtx);
    mutex_unlock(&pi_multi_mtx);
    atomic_fetch_add(&pi_multi_done, 1);
}

TEST_DECLARE_INTEGRATION(mutex_pi_multi_waiters, .group = TEST_GROUP(mutex),
                         TEST_INTENSITY_LINEAR(2, 2, 8, "rt_waiters")) {
    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    size_t num_rt = ctx->intensity_val ? ctx->intensity_val : 2;
    if (num_rt < 1)
        num_rt = 1;
    if (num_rt > 8)
        num_rt = 8;

    atomic_store(&pi_multi_done, 0);
    atomic_store(&ts_got, false);

    cpu_id_t cpu = 1;

    struct thread *ts = thread_create("pi_ts", pi_multi_ts, NULL);
    struct thread *rt[8];
    for (size_t i = 0; i < num_rt; i++) {
        rt[i] = thread_create("pi_rt", pi_multi_rt, NULL);
        rt[i]->perceived_prio_class = THREAD_PRIO_CLASS_RT;
        thread_pin(rt[i]);
        thread_set_joinable(rt[i]);
    }

    thread_pin(ts);
    thread_set_joinable(ts);

    thread_enqueue_on_core(ts, cpu);
    while (!atomic_load(&ts_got))
        scheduler_yield();

    for (size_t i = 0; i < num_rt; i++)
        thread_enqueue_on_core(rt[i], cpu);

    thread_join(ts);
    for (size_t i = 0; i < num_rt; i++)
        thread_join(rt[i]);

    TEST_ASSERT(atomic_load(&pi_multi_done) == (unsigned) (num_rt + 1));

    return TEST_SUCCESS;
}

static struct mutex pi_revert_mtx = MUTEX_INIT;
static atomic_bool pi_reverted = false;
static atomic_bool pi_revert_got = false;
static atomic_uint pi_reverted_done = 0;

static void pi_revert_ts(void *arg) {
    (void) arg;
    mutex_lock(&pi_revert_mtx);

    atomic_store(&pi_revert_got, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    mutex_unlock(&pi_revert_mtx);

    while (thread_get_current()->perceived_prio_class == THREAD_PRIO_CLASS_RT)
        cpu_relax();

    atomic_store(&pi_reverted, true);
    atomic_fetch_add(&pi_reverted_done, 1);
}

static void pi_revert_rt(void *arg) {
    (void) arg;
    mutex_lock(&pi_revert_mtx);
    mutex_unlock(&pi_revert_mtx);
    atomic_fetch_add(&pi_reverted_done, 1);
}

TEST_DECLARE_INTEGRATION(mutex_pi_revert, .group = TEST_GROUP(mutex)) {
    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    atomic_store(&pi_reverted, false);
    atomic_store(&pi_revert_got, false);
    atomic_store(&pi_reverted_done, 0);

    cpu_id_t cpu = 1;

    struct thread *ts = thread_create("pi_ts", pi_revert_ts, NULL);
    struct thread *rt = thread_create("pi_rt", pi_revert_rt, NULL);

    rt->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_pin(ts);
    thread_pin(rt);

    thread_set_joinable(ts);
    thread_set_joinable(rt);

    thread_enqueue_on_core(ts, cpu);
    while (!atomic_load(&pi_revert_got))
        scheduler_yield();

    thread_enqueue_on_core(rt, cpu);

    thread_join(ts);
    thread_join(rt);

    TEST_ASSERT(atomic_load(&pi_reverted));

    return TEST_SUCCESS;
}
#endif
