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

REGISTER_TEST(mutex_test_basic, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    MUTEX_REPORT_PROBLEMS();

    mutex_lock(&basic_test_mtx);
    scheduler_yield();
    mutex_unlock(&basic_test_mtx);
    SET_SUCCESS();
}

#define MUTEX_MANY_WAITER_TEST_WAITER_COUNT 67
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

REGISTER_TEST(mutex_many_waiters, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < MUTEX_MANY_WAITER_TEST_WAITER_COUNT; i++) {
        struct thread *t = thread_create("mw", many_worker, NULL);
        t->flags = THREAD_FLAGS_NO_STEAL;
        scheduler_enqueue(t);
    }

    while (atomic_load(&many_waiter_done))
        scheduler_yield();

    SET_SUCCESS();
}

#define CHAOS_THREAD_COUNT 20

static struct mutex chaos_mtx = MUTEX_INIT;
static _Atomic uint32_t chaos_left = CHAOS_THREAD_COUNT;

static void chaos(void *) {
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
    MUTEX_REPORT_PROBLEMS();

    main_thread = scheduler_get_current_thread();
    for (int i = 0; i < CHAOS_THREAD_COUNT; i++)
        other_threads[i] = thread_spawn("ch", chaos, NULL);

    while (atomic_load(&chaos_left))
        scheduler_yield();

    SET_SUCCESS();
}

#endif
