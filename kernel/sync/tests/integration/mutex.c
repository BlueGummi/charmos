#include "sync/tests/test_internal.h"

#ifdef TEST_MUTEX

#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

#define MUTEX_MANY_WAITER_MAX 64
#define MUTEX_MANY_WAITER_LOOP_COUNT 500

static struct mutex many_mtx = MUTEX_INIT;
static _Atomic uint32_t many_waiter_done = 0;

static void many_worker(void *) {
    for (int i = 0; i < MUTEX_MANY_WAITER_LOOP_COUNT; i++) {
        mutex_lock(&many_mtx);
        scheduler_yield();
        mutex_unlock(&many_mtx);
    }

    atomic_fetch_sub(&many_waiter_done, 1);
}

TEST_DECLARE_INTEGRATION(mutex, many_waiters, TEST_INTENSITY(2, 10, 32)) {
    size_t num_waiters = ctx->intensity_val ? ctx->intensity_val : 10;
    if (num_waiters > MUTEX_MANY_WAITER_MAX)
        num_waiters = MUTEX_MANY_WAITER_MAX;

    atomic_store(&many_waiter_done, (uint32_t) num_waiters);
    struct thread *workers[MUTEX_MANY_WAITER_MAX];

    for (size_t i = 0; i < num_waiters; i++) {
        struct thread *t = thread_create("mw", many_worker, NULL);
        TEST_ASSERT_NONNULL(t);

        thread_pin(t);
        thread_set_joinable(t);
        thread_enqueue(t);
        workers[i] = t;
    }

    for (size_t i = 0; i < num_waiters; i++)
        thread_join(workers[i]);

    TEST_ASSERT_EQ(atomic_load(&many_waiter_done), 0);

    return TEST_SUCCESS;
}

#define CHAOS_THREAD_MAX 64
#define CHAOS_LOOPS 500

static struct mutex chaos_mtx = MUTEX_INIT;
static _Atomic uint32_t chaos_left = 0;

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
struct thread *other_threads[CHAOS_THREAD_MAX] = {0};

TEST_DECLARE_INTEGRATION(mutex, chaos, TEST_INTENSITY(20, 50, 100)) {
    size_t num_threads = ctx->intensity_val ? ctx->intensity_val : 24;
    if (num_threads > CHAOS_THREAD_MAX)
        num_threads = CHAOS_THREAD_MAX;

    atomic_store(&chaos_left, (uint32_t) num_threads);
    main_thread = thread_get_current();
    for (size_t i = 0; i < num_threads; i++) {
        other_threads[i] = thread_spawn_joinable("ch", chaos, NULL);
        TEST_ASSERT_NONNULL(other_threads[i]);
    }

    for (size_t i = 0; i < num_threads; i++)
        thread_join(other_threads[i]);

    TEST_ASSERT_EQ(atomic_load(&chaos_left), 0);

    return TEST_SUCCESS;
}
#endif
