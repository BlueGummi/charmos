#include <crypto/prng.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sync/mutex.h>
#include <tests.h>

static struct mutex basic_test_mtx = MUTEX_INIT;

REGISTER_TEST(mutex_test_basic, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    mutex_lock(&basic_test_mtx);
    scheduler_yield();
    mutex_unlock(&basic_test_mtx);
    SET_SUCCESS();
}

static struct mutex ct_mtx = MUTEX_INIT;

/*
static void contender() {
    mutex_lock(&ct_mtx);
    mutex_unlock(&ct_mtx);
}

REGISTER_TEST(mutex_two_thread_basic, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    mutex_lock(&ct_mtx);

    thread_spawn_on_core("contender", contender, 0);

    scheduler_yield();

    mutex_unlock(&ct_mtx);

    scheduler_yield();

    SET_SUCCESS();
}

static struct mutex many_mtx = MUTEX_INIT;

static void many_worker() {
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&many_mtx);
        scheduler_yield(); // force other threads into contention
        mutex_unlock(&many_mtx);
    }
}

REGISTER_TEST(mutex_many_waiters, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < 10; i++)
        thread_spawn_on_core("mw", many_worker, 0);

    for (int i = 0; i < 2000; i++)
        scheduler_yield();

    SET_SUCCESS();
}

static struct mutex ts_mtx = MUTEX_INIT;
#define MUTEX_TURNSTILE_SLOWPATH_WAITERS 3
static atomic_uint waiters_left = MUTEX_TURNSTILE_SLOWPATH_WAITERS;

static void waiter() {
    mutex_lock(&ts_mtx);
    mutex_unlock(&ts_mtx);
    atomic_fetch_sub(&waiters_left, 1);
}

REGISTER_TEST(mutex_turnstile_slowpath, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    mutex_lock(&ts_mtx);

    // spawn waiters; they'll block
    for (int i = 0; i < MUTEX_TURNSTILE_SLOWPATH_WAITERS; i++)
        thread_spawn_on_core("wt", waiter, 0);

    scheduler_yield(); // nobody can progress, still blocked

    thread_sleep_for_ms(67);

    mutex_unlock(&ts_mtx);

    scheduler_yield(); // waiters wake up now

    while (atomic_load(&waiters_left))
        scheduler_yield();

    SET_SUCCESS();
}

static struct mutex chaos_mtx = MUTEX_INIT;

static void chaos() {
    for (int i = 0; i < 2000; i++) {
        mutex_lock(&chaos_mtx);
        for (volatile size_t j = 0; j < (prng_next() & 0x1F); j++)
            ;
        mutex_unlock(&chaos_mtx);

        if (prng_next() & 1)
            scheduler_yield();
    }
}

REGISTER_TEST(mutex_chaos, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < 20; i++)
        thread_spawn("ch", chaos);

    for (int i = 0; i < 5000; i++)
        scheduler_yield();

    SET_SUCCESS();
}*/
