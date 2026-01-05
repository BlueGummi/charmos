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

#endif
