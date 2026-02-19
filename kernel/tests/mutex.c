#ifdef TEST_MUTEX

#include <crypto/prng.h>
#include <sch/sched.h>
#include <sync/mutex.h>
#include <tests.h>
#include <thread/thread.h>

#define MUTEX_REPORT_PROBLEMS()                                                \
    ADD_MESSAGE("Mutex tests are encountering problems and will be skipped");  \
    SET_SKIP();                                                                \
    return;

static struct mutex basic_test_mtx = MUTEX_INIT;

TEST_REGISTER(mutex_test_basic, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    mutex_lock(&basic_test_mtx);
    scheduler_yield();
    mutex_unlock(&basic_test_mtx);
    SET_SUCCESS();
}

#define MUTEX_MANY_WAITER_TEST_WAITER_COUNT 10
#define MUTEX_MANY_WAITER_LOOP_COUNT 500

static struct mutex many_mtx = MUTEX_INIT;
static _Atomic uint32_t many_waiter_done = MUTEX_MANY_WAITER_TEST_WAITER_COUNT;

static void many_worker(void *) {
    for (int i = 0; i < MUTEX_MANY_WAITER_LOOP_COUNT; i++) {
        mutex_lock(&many_mtx);
        scheduler_yield();
        mutex_unlock(&many_mtx);
    }

    atomic_fetch_sub(&many_waiter_done, 1);
}

TEST_REGISTER(mutex_many_waiters, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < MUTEX_MANY_WAITER_TEST_WAITER_COUNT; i++) {
        struct thread *t = thread_create("mw", many_worker, NULL);
        t->flags = THREAD_FLAGS_NO_STEAL;
        scheduler_enqueue(t);
    }

    while (atomic_load(&many_waiter_done))
        scheduler_yield();

    SET_SUCCESS();
}

#define CHAOS_THREAD_COUNT 24
#define CHAOS_LOOPS 500

static struct mutex chaos_mtx = MUTEX_INIT;
static _Atomic uint32_t chaos_left = CHAOS_THREAD_COUNT;

static void chaos(void *) {
    for (int i = 0; i < CHAOS_LOOPS; i++) {
        mutex_lock(&chaos_mtx);

        for (volatile size_t j = 0; j < (prng_next() & 0x1F); j++)
            cpu_relax();

        mutex_unlock(&chaos_mtx);

        if (prng_next() & 1)
            scheduler_yield();
    }

    atomic_fetch_sub(&chaos_left, 1);
}

volatile struct thread *main_thread = NULL;
volatile struct thread *other_threads[CHAOS_THREAD_COUNT] = {0};

TEST_REGISTER(mutex_chaos, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    main_thread = scheduler_get_current_thread();
    for (int i = 0; i < CHAOS_THREAD_COUNT; i++)
        other_threads[i] = thread_spawn("ch", chaos, NULL);

    while (atomic_load(&chaos_left))
        scheduler_yield();

    SET_SUCCESS();
}

/* we want to spawn a timesharing thread on another core, and
 * acquire a mutex with it. then we want to spawn a realtime thread
 * on the same core. the expected behavior is that the timesharing
 * thread gets boosted to the realtime priority class, allowing it to run
 * until it drops the lock */

static struct mutex pi_mutex = MUTEX_INIT;
static struct thread *pi_ts, *pi_rt, *pi_dum;
static atomic_bool pi_ts_got = false;
static atomic_uint pi_done = 0;

static void pi_dummy(void *nothing) {
    (void) nothing;
    k_log("dummy\n");
    while (atomic_load(&pi_done) < 1)
        scheduler_yield();

    atomic_fetch_add(&pi_done, 1);
    k_log("exiting\n");
}

static void pi_rt_thread(void *nothing) {
    (void) nothing;
    mutex_lock(&pi_mutex);
    k_log("lock\n");
    kassert(mutex_get_owner(&pi_mutex) == scheduler_get_current_thread());
    mutex_unlock(&pi_mutex);
    k_log("unlock\n");
    atomic_fetch_add(&pi_done, 1);
    k_log("exiting\n");
}

static void pi_ts_thread(void *nothing) {
    (void) nothing;
    mutex_lock(&pi_mutex);
    k_log("lock\n");
    atomic_store(&pi_ts_got, true);

    while (scheduler_get_current_thread()->perceived_prio_class !=
           THREAD_PRIO_CLASS_RT)
        cpu_relax();

    kassert(mutex_get_owner(&pi_mutex) == scheduler_get_current_thread());
    k_log("boosted\n");

    k_log("unlock\n");
    mutex_unlock(&pi_mutex);

    atomic_fetch_add(&pi_done, 1);
    k_log("exiting\n");
}

TEST_REGISTER(mutex_pi_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    if (global.core_count == 1) {
        SET_SKIP();
        return;
    }

    cpu_id_t cpu = 1;
    pi_ts = thread_create("pi_ts", pi_ts_thread, NULL);
    pi_rt = thread_create("pi_rt", pi_rt_thread, NULL);
    pi_dum = thread_create("pi_dum", pi_dummy, NULL);
    pi_rt->perceived_prio_class = THREAD_PRIO_CLASS_RT;
    pi_dum->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_set_flags(pi_dum, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(pi_ts, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(pi_rt, THREAD_FLAGS_NO_STEAL);

    scheduler_enqueue_on_core(pi_ts, cpu);
    while (!atomic_load(&pi_ts_got))
        scheduler_yield();

    scheduler_enqueue_on_core(pi_dum, cpu);
    scheduler_enqueue_on_core(pi_rt, cpu);

    while (atomic_load(&pi_done) < 3)
        scheduler_yield();

    SET_SUCCESS();
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
    k_log("ts2 lock b\n");
    atomic_store(&ts2_grabbed_b, true);

    /* wait until boosted */
    while (scheduler_get_current_thread()->perceived_prio_class !=
           THREAD_PRIO_CLASS_RT)
        cpu_relax();

    k_log("ts2 boosted\n");
    mutex_unlock(&pi_mtx_b);
    atomic_fetch_add(&pi_chain_done, 1);
}

static void pi_chain_ts1(void *arg) {
    (void) arg;
    mutex_lock(&pi_mtx_a);
    k_log("ts1 lock a\n");
    atomic_store(&ts1_grabbed_a, true);
    
    /* wait until boosted */
    while (scheduler_get_current_thread()->perceived_prio_class !=
           THREAD_PRIO_CLASS_RT)
        cpu_relax();

    mutex_lock(&pi_mtx_b);
    k_log("ts1 lock b\n");

    mutex_unlock(&pi_mtx_b);
    mutex_unlock(&pi_mtx_a);
    atomic_fetch_add(&pi_chain_done, 1);
}

static void pi_chain_rt(void *arg) {
    (void) arg;
    k_log("rt lock\n");
    mutex_lock(&pi_mtx_a);
    k_log("rt lock got\n");
    
    mutex_unlock(&pi_mtx_a);
    atomic_fetch_add(&pi_chain_done, 1);
}

TEST_REGISTER(mutex_pi_chain, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    if (global.core_count < 2) {
        SET_SKIP();
        return;
    }

    cpu_id_t cpu = 1;

    pi_ts2 = thread_create("pi_ts2", pi_chain_ts2, NULL);
    pi_ts1 = thread_create("pi_ts1", pi_chain_ts1, NULL);
    pi_rt2 = thread_create("pi_rt2", pi_chain_rt, NULL);

    pi_rt2->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_set_flags(pi_ts1, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(pi_ts2, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(pi_rt2, THREAD_FLAGS_NO_STEAL);

    scheduler_enqueue_on_core(pi_ts2, cpu);
    while (!atomic_load(&ts2_grabbed_b))
        scheduler_yield();

    scheduler_enqueue_on_core(pi_ts1, cpu);

    /* let ts1 grab A and block on B */
    while (!atomic_load(&ts1_grabbed_a))
        scheduler_yield();

    scheduler_enqueue_on_core(pi_rt2, cpu);

    while (atomic_load(&pi_chain_done) < 3)
        scheduler_yield();

    SET_SUCCESS();
}

static struct mutex pi_multi_mtx = MUTEX_INIT;
static atomic_uint pi_multi_done = 0;
static atomic_bool ts_got = false;

static void pi_multi_ts(void *arg) {
    (void) arg;
    mutex_lock(&pi_multi_mtx);
    k_log("multi_ts running\n");
    atomic_store(&ts_got, true);

    while (scheduler_get_current_thread()->perceived_prio_class !=
           THREAD_PRIO_CLASS_RT)
        cpu_relax();

    k_log("ts boosted\n");
    mutex_unlock(&pi_multi_mtx);
    atomic_fetch_add(&pi_multi_done, 1);
}

static void pi_multi_rt(void *arg) {
    (void) arg;
    k_log("multi_rt running\n");
    mutex_lock(&pi_multi_mtx);
    mutex_unlock(&pi_multi_mtx);
    atomic_fetch_add(&pi_multi_done, 1);
}

TEST_REGISTER(mutex_pi_multi_waiters, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    cpu_id_t cpu = 1;

    struct thread *ts = thread_create("pi_ts", pi_multi_ts, NULL);
    struct thread *rt1 = thread_create("pi_rt1", pi_multi_rt, NULL);
    struct thread *rt2 = thread_create("pi_rt2", pi_multi_rt, NULL);

    rt1->perceived_prio_class = THREAD_PRIO_CLASS_RT;
    rt2->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_set_flags(ts, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(rt1, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(rt2, THREAD_FLAGS_NO_STEAL);

    scheduler_enqueue_on_core(ts, cpu);
    while (!atomic_load(&ts_got))
        scheduler_yield();

    scheduler_enqueue_on_core(rt1, cpu);
    scheduler_enqueue_on_core(rt2, cpu);

    while (atomic_load(&pi_multi_done) < 3)
        scheduler_yield();

    SET_SUCCESS();
}

static struct mutex pi_revert_mtx = MUTEX_INIT;
static atomic_bool pi_reverted = false;
static atomic_bool pi_revert_got = false;

static void pi_revert_ts(void *arg) {
    (void) arg;
    mutex_lock(&pi_revert_mtx);

    atomic_store(&pi_revert_got, true);

    while (scheduler_get_current_thread()->perceived_prio_class !=
           THREAD_PRIO_CLASS_RT)
        cpu_relax();

    mutex_unlock(&pi_revert_mtx);

    while (scheduler_get_current_thread()->perceived_prio_class ==
           THREAD_PRIO_CLASS_RT)
        cpu_relax();

    atomic_store(&pi_reverted, true);
}

static void pi_revert_rt(void *arg) {
    (void) arg;
    mutex_lock(&pi_revert_mtx);
    mutex_unlock(&pi_revert_mtx);
}

TEST_REGISTER(mutex_pi_revert, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    cpu_id_t cpu = 1;

    struct thread *ts = thread_create("pi_ts", pi_revert_ts, NULL);
    struct thread *rt = thread_create("pi_rt", pi_revert_rt, NULL);

    rt->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    thread_set_flags(ts, THREAD_FLAGS_NO_STEAL);
    thread_set_flags(rt, THREAD_FLAGS_NO_STEAL);

    scheduler_enqueue_on_core(ts, cpu);

    while (!atomic_load(&pi_revert_got))
        scheduler_yield();

    scheduler_enqueue_on_core(rt, cpu);

    while (!atomic_load(&pi_reverted))
        scheduler_yield();

    SET_SUCCESS();
}

#endif
