#ifdef TEST_RWLOCK

#include <crypto/prng.h>
#include <sch/sched.h>
#include <thread/thread.h>

#include <stdatomic.h>
#include <sync/rwlock.h>
#include <tests.h>

#define RWLOCK_REPORT_PROBLEMS()                                               \
    ADD_MESSAGE("rwlock tests are encountering problems and will be skipped"); \
    SET_SKIP();                                                                \
    return;

static struct rwlock rw_basic = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_REGISTER(rwlock_basic_read, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    rwlock_lock(&rw_basic, RWLOCK_ACQUIRE_READ);
    scheduler_yield();
    rwlock_unlock(&rw_basic);

    SET_SUCCESS();
}

static struct rwlock rw_basic_w = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_REGISTER(rwlock_basic_write, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    rwlock_lock(&rw_basic_w, RWLOCK_ACQUIRE_WRITE);
    scheduler_yield();
    rwlock_unlock(&rw_basic_w);

    SET_SUCCESS();
}

static struct rwlock rw_two_writers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static atomic_bool rw_two_done = false;

static void rw_two_writer_thread(void *) {
    rwlock_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);
    rwlock_unlock(&rw_two_writers);

    atomic_store(&rw_two_done, true);
}

TEST_REGISTER(rwlock_two_writer_basic, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    rwlock_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);

    thread_spawn_on_core("rw_two_writer", rw_two_writer_thread, NULL, 0);

    scheduler_yield(); // let second writer block

    rwlock_unlock(&rw_two_writers);

    while (!atomic_load(&rw_two_done))
        scheduler_yield();

    SET_SUCCESS();
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
            printf("RWlock reader %s on iteration %zu\n",
                     thread_get_current()->name, i);
        }
        last_print = now;
    }

    atomic_fetch_sub(&rw_readers_left, 1);
}

TEST_REGISTER(rwlock_many_readers, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < RWLOCK_READER_COUNT_TEST_N; i++)
        thread_spawn("rr_%zu", rw_reader_worker, NULL, i);
    irql_lower(irql);

    while (atomic_load(&rw_readers_left))
        scheduler_yield();

    SET_SUCCESS();
}

#define RWLOCK_MIXED_THREADS 24
#define RWLOCK_MIXED_LOOPS 500
volatile struct thread *mixed_threads[RWLOCK_MIXED_THREADS];
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

TEST_REGISTER(rwlock_mixed_stress, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {

    for (int i = 0; i < RWLOCK_MIXED_THREADS; i++)
        mixed_threads[i] = thread_spawn("rm", rw_mixed_worker, NULL);

    while (atomic_load(&rw_mixed_left))
        scheduler_yield();

    SET_SUCCESS();
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

    printf("%u threads left\n", atomic_fetch_sub(&rw_chaos_left, 1) - 1);
}

TEST_REGISTER(rwlock_chaos, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < RWLOCK_CHAOS_THREADS; i++)
        thread_spawn("rch", rw_chaos_worker, NULL);
    irql_lower(irql);

    while (atomic_load(&rw_chaos_left)) {
        thread_apply_cpu_penalty(thread_get_current());
        scheduler_yield();
    }

    SET_SUCCESS();
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

TEST_REGISTER(rwlock_correctness, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {

    for (int i = 0; i < RWLOCK_CORRECT_THREADS; i++)
        thread_spawn("rwc", rw_correct_worker, NULL);

    while (!atomic_load(&correctness_left))
        scheduler_yield();

    while (!atomic_load(&correctness_ok))
        scheduler_yield();

    SET_SUCCESS();
}

#endif
