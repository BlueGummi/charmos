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

#define MUTEX_MANY_WAITER_TEST_WAITER_COUNT 10

static struct mutex many_mtx = MUTEX_INIT;
static _Atomic uint32_t many_waiter_done = MUTEX_MANY_WAITER_TEST_WAITER_COUNT;

static void many_worker() {
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&many_mtx);
        scheduler_yield();
        mutex_unlock(&many_mtx);
    }

    atomic_fetch_sub(&many_waiter_done, 1);
}

REGISTER_TEST(mutex_many_waiters, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < MUTEX_MANY_WAITER_TEST_WAITER_COUNT; i++)
        thread_spawn_on_core("mw", many_worker, 0);

    while (atomic_load(&many_waiter_done))
        scheduler_yield();

    SET_SUCCESS();
}

#define CHAOS_THREAD_COUNT 20

static struct mutex chaos_mtx = MUTEX_INIT;
static _Atomic uint32_t chaos_left = CHAOS_THREAD_COUNT;

static void chaos() {
    for (int i = 0; i < 2000; i++) {
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

REGISTER_TEST(mutex_chaos, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    main_thread = scheduler_get_current_thread();
    for (int i = 0; i < CHAOS_THREAD_COUNT; i++)
        other_threads[i] = thread_spawn("ch", chaos);

    while (atomic_load(&chaos_left))
        scheduler_yield();

    SET_SUCCESS();
}
