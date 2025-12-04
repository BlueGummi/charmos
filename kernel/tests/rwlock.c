#include <crypto/prng.h>
#include <sch/sched.h>
#include <sch/thread.h>

#include <stdatomic.h>
#include <sync/rwlock.h>
#include <tests.h>

static struct rwlock rw_basic = {0};

REGISTER_TEST(rwlock_basic_read, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    rwlock_lock(&rw_basic, RWLOCK_ACQUIRE_READ);
    scheduler_yield();
    rwlock_unlock(&rw_basic);

    SET_SUCCESS();
}

static struct rwlock rw_basic_w = {0};

REGISTER_TEST(rwlock_basic_write, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    rwlock_lock(&rw_basic_w, RWLOCK_ACQUIRE_WRITE);
    scheduler_yield();
    rwlock_unlock(&rw_basic_w);

    SET_SUCCESS();
}

static struct rwlock rw_two_writers = {0};
static atomic_bool rw_two_done = false;

static void rw_two_writer_thread(void *) {
    rwlock_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);
    rwlock_unlock(&rw_two_writers);

    atomic_store(&rw_two_done, true);
}

REGISTER_TEST(rwlock_two_writer_basic, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    rwlock_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);

    thread_spawn_on_core("rw_two_writer", rw_two_writer_thread, NULL, 0);

    scheduler_yield(); // let second writer block

    rwlock_unlock(&rw_two_writers);

    while (!atomic_load(&rw_two_done))
        scheduler_yield();

    SET_SUCCESS();
}

#define RWLOCK_READER_COUNT_TEST_N 10

static struct rwlock rw_readers = {0};
static _Atomic uint32_t rw_readers_left = RWLOCK_READER_COUNT_TEST_N;

static void rw_reader_worker(void *) {
    for (int i = 0; i < 1000; i++) {
        rwlock_lock(&rw_readers, RWLOCK_ACQUIRE_READ);
        scheduler_yield();
        rwlock_unlock(&rw_readers);
    }

    atomic_fetch_sub(&rw_readers_left, 1);
}

REGISTER_TEST(rwlock_many_readers, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < RWLOCK_READER_COUNT_TEST_N; i++)
        thread_spawn("rr", rw_reader_worker, NULL);

    while (atomic_load(&rw_readers_left))
        scheduler_yield();

    SET_SUCCESS();
}

#define RWLOCK_MIXED_THREADS 12
volatile struct thread *mixed_threads[RWLOCK_MIXED_THREADS];
static struct rwlock rw_mixed = {0};
static _Atomic uint32_t rw_mixed_left = RWLOCK_MIXED_THREADS;

static void rw_mixed_worker(void *) {
    for (int i = 0; i < 1500; i++) {
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

REGISTER_TEST(rwlock_mixed_stress, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < RWLOCK_MIXED_THREADS; i++)
        mixed_threads[i] = thread_spawn("rm", rw_mixed_worker, NULL);

    while (atomic_load(&rw_mixed_left))
        scheduler_yield();

    SET_SUCCESS();
}

#define RWLOCK_CHAOS_THREADS 20

static struct rwlock rw_chaos = {0};
static _Atomic uint32_t rw_chaos_left = RWLOCK_CHAOS_THREADS;

static void rw_chaos_worker(void *) {
    for (int i = 0; i < 3000; i++) {
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

    atomic_fetch_sub(&rw_chaos_left, 1);
}

REGISTER_TEST(rwlock_chaos, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < RWLOCK_CHAOS_THREADS; i++)
        thread_spawn("rch", rw_chaos_worker, NULL);

    while (atomic_load(&rw_chaos_left)) {
        thread_apply_cpu_penalty(scheduler_get_current_thread());
        scheduler_yield();
    }

    SET_SUCCESS();
}

static struct rwlock rw_correct = {0};
static _Atomic uint32_t active_readers = 0;

static _Atomic uint32_t active_writers = 0;
static atomic_bool correctness_ok = true;

static void rw_correct_worker(void *) {
    for (int i = 0; i < 2000; i++) {
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
}

#define RWLOCK_CORRECT_THREADS 8

REGISTER_TEST(rwlock_correctness, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    for (int i = 0; i < RWLOCK_CORRECT_THREADS; i++)
        thread_spawn("rwc", rw_correct_worker, NULL);

    while (!atomic_load(&correctness_ok))
        scheduler_yield();

    SET_SUCCESS();
}
