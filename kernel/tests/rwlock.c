#ifdef TEST_RWLOCK

#include <crypto/prng.h>
#include <sch/sched.h>
#include <thread/thread.h>

#include <stdatomic.h>
#include <sync/rwlock.h>
#include <test.h>

TEST_GROUP_DECLARE(rwlock);

#define RWLOCK_REPORT_PROBLEMS()                                               \
    test_info("rwlock tests are encountering problems and will be skipped");   \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct rwlock rw_basic = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_UNIT(rwlock_basic_read, .group = TEST_GROUP(rwlock)) {
    rwlock_lock(&rw_basic, RWLOCK_ACQUIRE_READ);
    scheduler_yield();
    rwlock_unlock(&rw_basic);

    return TEST_SUCCESS;
}

static struct rwlock rw_basic_w = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_UNIT(rwlock_basic_write, .group = TEST_GROUP(rwlock)) {

    rwlock_lock(&rw_basic_w, RWLOCK_ACQUIRE_WRITE);
    scheduler_yield();
    rwlock_unlock(&rw_basic_w);

    return TEST_SUCCESS;
}

static struct rwlock rw_two_writers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static atomic_bool rw_two_done = false;

static void rw_two_writer_thread(void *) {
    rwlock_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);
    rwlock_unlock(&rw_two_writers);

    atomic_store(&rw_two_done, true);
}

TEST_DECLARE_UNIT(rwlock_two_writer_basic, .group = TEST_GROUP(rwlock)) {

    rwlock_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);

    struct thread *w = thread_spawn_joinable_on_core(
        "rw_two_writer", rw_two_writer_thread, NULL, 0);
    TEST_ASSERT(w);

    scheduler_yield(); // let second writer block

    rwlock_unlock(&rw_two_writers);

    thread_join(w);
    TEST_ASSERT(atomic_load(&rw_two_done));

    return TEST_SUCCESS;
}

#define RWLOCK_READER_COUNT_TEST_N 20
#define RWLOCK_READER_COUNT_LOOPS 500
#define RWLOCK_READER_PRINT_INTERVAL 10000

static struct rwlock rw_readers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_readers_left = RWLOCK_READER_COUNT_TEST_N;

static void rw_reader_worker(void *) {
    time_t last_print = time_get_ms();
    for (size_t i = 0; i < RWLOCK_READER_COUNT_LOOPS; i++) {
        rwlock_lock(&rw_readers, RWLOCK_ACQUIRE_READ);
        scheduler_yield();
        rwlock_unlock(&rw_readers);
        time_t now = time_get_ms();
        if ((now - last_print) > RWLOCK_READER_PRINT_INTERVAL) {
            test_info("RWlock reader %s on iteration %zu",
                      thread_get_current()->name, i);
        }
        last_print = now;
    }

    atomic_fetch_sub(&rw_readers_left, 1);
}

TEST_DECLARE_INTEGRATION(rwlock_many_readers, .group = TEST_GROUP(rwlock),
                         .print_logs = true) {
    struct thread *readers[RWLOCK_READER_COUNT_TEST_N];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < RWLOCK_READER_COUNT_TEST_N; i++)
        readers[i] = thread_spawn_joinable("rr_%zu", rw_reader_worker, NULL, i);
    irql_lower(irql);

    for (int i = 0; i < RWLOCK_READER_COUNT_TEST_N; i++) {
        if (readers[i])
            thread_join(readers[i]);
    }

    /* a failed spawn leaves the counter short, which this catches */
    TEST_ASSERT(atomic_load(&rw_readers_left) == 0);

    return TEST_SUCCESS;
}

#define RWLOCK_MIXED_THREADS 24
#define RWLOCK_MIXED_LOOPS 500
struct thread *mixed_threads[RWLOCK_MIXED_THREADS];
static struct rwlock rw_mixed = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_mixed_left = RWLOCK_MIXED_THREADS;

static void rw_mixed_worker(void *) {
    for (int i = 0; i < RWLOCK_MIXED_LOOPS; i++) {
        if (prng_next() & 1) {
            // Reader
            rwlock_lock(&rw_mixed, RWLOCK_ACQUIRE_READ);
        } else {
            // Writer
            rwlock_lock(&rw_mixed, RWLOCK_ACQUIRE_WRITE);
        }

        for (volatile size_t j = 0; j < (prng_next() & 0x1f); j++)
            cpu_relax();

        rwlock_unlock(&rw_mixed);

        if (prng_next() & 1)
            scheduler_yield();
    }

    atomic_fetch_sub(&rw_mixed_left, 1);
}

TEST_DECLARE_INTEGRATION(rwlock_mixed_stress, .group = TEST_GROUP(rwlock)) {

    for (int i = 0; i < RWLOCK_MIXED_THREADS; i++)
        mixed_threads[i] = thread_spawn_joinable("rm", rw_mixed_worker, NULL);

    for (int i = 0; i < RWLOCK_MIXED_THREADS; i++) {
        if (mixed_threads[i])
            thread_join(mixed_threads[i]);
    }

    TEST_ASSERT(atomic_load(&rw_mixed_left) == 0);

    return TEST_SUCCESS;
}

#define RWLOCK_CHAOS_THREADS 24
#define RWLOCK_CHAOS_LOOPS 500

static struct rwlock rw_chaos = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_chaos_left = RWLOCK_CHAOS_THREADS;

static void rw_chaos_worker(void *) {
    for (int i = 0; i < RWLOCK_CHAOS_LOOPS; i++) {
        if (prng_next() & 1)
            rwlock_lock(&rw_chaos, RWLOCK_ACQUIRE_READ);
        else
            rwlock_lock(&rw_chaos, RWLOCK_ACQUIRE_WRITE);

        for (volatile size_t j = 0; j < (prng_next() & 0x1F); j++)
            cpu_relax();

        rwlock_unlock(&rw_chaos);

        if (prng_next() & 1)
            scheduler_yield();
    }

    test_info("%u threads left", atomic_fetch_sub(&rw_chaos_left, 1) - 1);
}

TEST_DECLARE_INTEGRATION(rwlock_chaos, .group = TEST_GROUP(rwlock),
                         .print_logs = true) {
    struct thread *workers[RWLOCK_CHAOS_THREADS];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < RWLOCK_CHAOS_THREADS; i++)
        workers[i] = thread_spawn_joinable("rch", rw_chaos_worker, NULL);
    irql_lower(irql);

    /* blocking on the join means we no longer have to penalize ourselves
     * to keep the workers scheduled */
    for (int i = 0; i < RWLOCK_CHAOS_THREADS; i++) {
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
#define RWLOCK_CORRECT_THREADS 16

static atomic_uint correctness_left = RWLOCK_CORRECT_THREADS;

static void rw_correct_worker(void *) {
    for (int i = 0; i < RWLOCK_CORRECT_LOOPS; i++) {
        if (prng_next() & 1) {
            // Reader
            rwlock_lock(&rw_correct, RWLOCK_ACQUIRE_READ);

            atomic_fetch_add(&active_readers, 1);
            if (atomic_load(&active_writers) != 0)
                atomic_store(&correctness_ok, false);

            scheduler_yield();
            atomic_fetch_sub(&active_readers, 1);

            rwlock_unlock(&rw_correct);
        } else {
            // Writer

            rwlock_lock(&rw_correct, RWLOCK_ACQUIRE_WRITE);

            atomic_fetch_add(&active_writers, 1);
            if (atomic_load(&active_readers) != 0 ||
                atomic_load(&active_writers) > 1)
                atomic_store(&correctness_ok, false);

            scheduler_yield();
            atomic_fetch_sub(&active_writers, 1);

            rwlock_unlock(&rw_correct);
        }
    }
    atomic_fetch_sub(&correctness_left, 1);
}

TEST_DECLARE_INTEGRATION(rwlock_correctness, .group = TEST_GROUP(rwlock)) {

    struct thread *workers[RWLOCK_CORRECT_THREADS];

    for (int i = 0; i < RWLOCK_CORRECT_THREADS; i++)
        workers[i] = thread_spawn_joinable("rwc", rw_correct_worker, NULL);

    for (int i = 0; i < RWLOCK_CORRECT_THREADS; i++) {
        if (workers[i])
            thread_join(workers[i]);
    }

    TEST_ASSERT(atomic_load(&correctness_left) == 0);

    /* this used to spin on correctness_ok, which meant a detected
     * violation hung the test instead of failing it */
    TEST_ASSERT(atomic_load(&correctness_ok));

    return TEST_SUCCESS;
}

#endif
