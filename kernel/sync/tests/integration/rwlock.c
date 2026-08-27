#include "../test_internal.h"

#ifdef TEST_RWLOCK

static struct rwlock rw_two_writers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static atomic_bool rw_two_done = false;

static void rw_two_writer_thread(void *) {
    rw_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);
    rw_unlock(&rw_two_writers);

    atomic_store(&rw_two_done, true);
}

TEST_DECLARE_INTEGRATION(rwlock_two_writer_basic, .group = TEST_GROUP(rwlock)) {
    atomic_store(&rw_two_done, false);
    rw_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);

    struct thread *w = thread_spawn_joinable_on_core(
        "rw_two_writer", rw_two_writer_thread, NULL, 0);
    TEST_ASSERT(w);

    scheduler_yield(); // let second writer block

    rw_unlock(&rw_two_writers);

    thread_join(w);
    TEST_ASSERT(atomic_load(&rw_two_done));

    return TEST_SUCCESS;
}

#define RWLOCK_READER_MAX 64
#define RWLOCK_READER_COUNT_LOOPS 500
#define RWLOCK_READER_PRINT_INTERVAL 10000

static struct rwlock rw_readers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_readers_left = 0;

static void rw_reader_worker(void *) {
    time_ms_t last_print = time_get_ms();
    for (size_t i = 0; i < RWLOCK_READER_COUNT_LOOPS; i++) {
        rw_lock(&rw_readers, RWLOCK_ACQUIRE_READ);
        scheduler_yield();
        rw_unlock(&rw_readers);
        time_ms_t now = time_get_ms();
        if ((now - last_print) > RWLOCK_READER_PRINT_INTERVAL) {
            test_info("RWlock reader %s on iteration %zu",
                      thread_get_current()->name, i);
        }
        last_print = now;
    }

    atomic_fetch_sub(&rw_readers_left, 1);
}

TEST_DECLARE_INTEGRATION(rwlock_many_readers, .group = TEST_GROUP(rwlock),
                         TEST_INTENSITY(4, 20, 64)) {
    size_t num_readers = ctx->intensity_val ? ctx->intensity_val : 20;
    if (num_readers > RWLOCK_READER_MAX)
        num_readers = RWLOCK_READER_MAX;

    atomic_store(&rw_readers_left, (uint32_t) num_readers);
    struct thread *readers[RWLOCK_READER_MAX];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < num_readers; i++)
        readers[i] = thread_spawn_joinable("rr_%zu", rw_reader_worker, NULL, i);
    irql_lower(irql);

    for (size_t i = 0; i < num_readers; i++) {
        if (readers[i])
            thread_join(readers[i]);
    }

    /* a failed spawn leaves the counter short, which this catches */
    TEST_ASSERT(atomic_load(&rw_readers_left) == 0);

    return TEST_SUCCESS;
}

#define RWLOCK_MIXED_THREADS_MAX 64
#define RWLOCK_MIXED_LOOPS 500
struct thread *mixed_threads[RWLOCK_MIXED_THREADS_MAX];
static struct rwlock rw_mixed = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_mixed_left = 0;

static void rw_mixed_worker(void *) {
    for (int i = 0; i < RWLOCK_MIXED_LOOPS; i++) {
        if (prng_next() & 1) {
            // Reader
            rw_lock(&rw_mixed, RWLOCK_ACQUIRE_READ);
        } else {
            // Writer
            rw_lock(&rw_mixed, RWLOCK_ACQUIRE_WRITE);
        }

        for (volatile size_t j = 0; j < (prng_next() & 0x1f); j++)
            cpu_relax();

        rw_unlock(&rw_mixed);

        if (prng_next() & 1)
            scheduler_yield();
    }

    atomic_fetch_sub(&rw_mixed_left, 1);
}

TEST_DECLARE_INTEGRATION(rwlock_mixed_stress, .group = TEST_GROUP(rwlock),
                         TEST_INTENSITY(4, 24, 64)) {
    size_t num_threads = ctx->intensity_val ? ctx->intensity_val : 24;
    if (num_threads > RWLOCK_MIXED_THREADS_MAX)
        num_threads = RWLOCK_MIXED_THREADS_MAX;

    atomic_store(&rw_mixed_left, (uint32_t) num_threads);

    for (size_t i = 0; i < num_threads; i++)
        mixed_threads[i] = thread_spawn_joinable("rm", rw_mixed_worker, NULL);

    for (size_t i = 0; i < num_threads; i++) {
        if (mixed_threads[i])
            thread_join(mixed_threads[i]);
    }

    TEST_ASSERT(atomic_load(&rw_mixed_left) == 0);

    return TEST_SUCCESS;
}

#define RWLOCK_CHAOS_THREADS_MAX 64
#define RWLOCK_CHAOS_LOOPS 500

static struct rwlock rw_chaos = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_chaos_left = 0;

static void rw_chaos_worker(void *) {
    for (int i = 0; i < RWLOCK_CHAOS_LOOPS; i++) {
        if (prng_next() & 1)
            rw_lock(&rw_chaos, RWLOCK_ACQUIRE_READ);
        else
            rw_lock(&rw_chaos, RWLOCK_ACQUIRE_WRITE);

        for (volatile size_t j = 0; j < (prng_next() & 0x1F); j++)
            cpu_relax();

        rw_unlock(&rw_chaos);

        if (prng_next() & 1)
            scheduler_yield();
    }

    test_info("%u threads left", atomic_fetch_sub(&rw_chaos_left, 1) - 1);
}

TEST_DECLARE_INTEGRATION(rwlock_chaos, .group = TEST_GROUP(rwlock),
                         TEST_INTENSITY(4, 24, 64)) {
    size_t num_threads = ctx->intensity_val ? ctx->intensity_val : 24;
    if (num_threads > RWLOCK_CHAOS_THREADS_MAX)
        num_threads = RWLOCK_CHAOS_THREADS_MAX;

    atomic_store(&rw_chaos_left, (uint32_t) num_threads);
    struct thread *workers[RWLOCK_CHAOS_THREADS_MAX];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < num_threads; i++)
        workers[i] = thread_spawn_joinable("rch", rw_chaos_worker, NULL);
    irql_lower(irql);

    for (size_t i = 0; i < num_threads; i++) {
        if (workers[i])
            thread_join(workers[i]);
    }

    TEST_ASSERT(atomic_load(&rw_chaos_left) == 0);

    return TEST_SUCCESS;
}

static struct rwlock rw_correct = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t active_readers = 0;

static _Atomic uint32_t active_writers = 0;
static atomic_bool correctness_ok = true;

#define RWLOCK_CORRECT_LOOPS 5000
#define RWLOCK_CORRECT_THREADS_MAX 64

static atomic_uint correctness_left = 0;

static void rw_correct_worker(void *) {
    for (int i = 0; i < RWLOCK_CORRECT_LOOPS; i++) {
        if (prng_next() & 1) {
            // Reader
            rw_lock(&rw_correct, RWLOCK_ACQUIRE_READ);
            atomic_fetch_add(&active_readers, 1);

            if (atomic_load(&active_writers) > 0)
                atomic_store(&correctness_ok, false);

            for (volatile size_t j = 0; j < (prng_next() & 0xF); j++)
                cpu_relax();

            atomic_fetch_sub(&active_readers, 1);
            rw_unlock(&rw_correct);
        } else {
            // Writer
            rw_lock(&rw_correct, RWLOCK_ACQUIRE_WRITE);
            atomic_fetch_add(&active_writers, 1);

            if (atomic_load(&active_readers) > 0 ||
                atomic_load(&active_writers) > 1)
                atomic_store(&correctness_ok, false);

            for (volatile size_t j = 0; j < (prng_next() & 0xF); j++)
                cpu_relax();

            atomic_fetch_sub(&active_writers, 1);
            rw_unlock(&rw_correct);
        }

        if (prng_next() & 1)
            scheduler_yield();
    }

    atomic_fetch_sub(&correctness_left, 1);
}

TEST_DECLARE_INTEGRATION(rwlock_correctness, .group = TEST_GROUP(rwlock),
                         TEST_INTENSITY(4, 16, 64)) {
    size_t num_threads = ctx->intensity_val ? ctx->intensity_val : 16;
    if (num_threads > RWLOCK_CORRECT_THREADS_MAX)
        num_threads = RWLOCK_CORRECT_THREADS_MAX;

    atomic_store(&active_readers, 0);
    atomic_store(&active_writers, 0);
    atomic_store(&correctness_ok, true);
    atomic_store(&correctness_left, (unsigned) num_threads);

    struct thread *workers[RWLOCK_CORRECT_THREADS_MAX];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < num_threads; i++)
        workers[i] = thread_spawn_joinable("rcorr", rw_correct_worker, NULL);
    irql_lower(irql);

    for (size_t i = 0; i < num_threads; i++) {
        if (workers[i])
            thread_join(workers[i]);
    }

    TEST_ASSERT(atomic_load(&correctness_left) == 0);
    TEST_ASSERT(atomic_load(&correctness_ok));

    return TEST_SUCCESS;
}

#endif
