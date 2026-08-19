#include "../test_internal.h"

#ifdef TEST_MUTEX

#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct mutex basic_test_mtx = MUTEX_INIT;

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

TEST_DECLARE_INTEGRATION(mutex_many_waiters, .group = TEST_GROUP(mutex),
                         TEST_INTENSITY(2, 10, 32)) {
    size_t num_waiters = ctx->intensity_val ? ctx->intensity_val : 10;
    if (num_waiters > MUTEX_MANY_WAITER_MAX)
        num_waiters = MUTEX_MANY_WAITER_MAX;

    atomic_store(&many_waiter_done, (uint32_t) num_waiters);
    struct thread *workers[MUTEX_MANY_WAITER_MAX];

    for (size_t i = 0; i < num_waiters; i++) {
        struct thread *t = thread_create("mw", many_worker, NULL);
        TEST_ASSERT(t);

        thread_pin(t);
        thread_set_joinable(t);
        thread_enqueue(t);
        workers[i] = t;
    }

    for (size_t i = 0; i < num_waiters; i++)
        thread_join(workers[i]);

    TEST_ASSERT(atomic_load(&many_waiter_done) == 0);

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

TEST_DECLARE_INTEGRATION(mutex_chaos, .group = TEST_GROUP(mutex),
                         TEST_INTENSITY(4, 24, 64)) {
    size_t num_threads = ctx->intensity_val ? ctx->intensity_val : 24;
    if (num_threads > CHAOS_THREAD_MAX)
        num_threads = CHAOS_THREAD_MAX;

    atomic_store(&chaos_left, (uint32_t) num_threads);
    main_thread = thread_get_current();
    for (size_t i = 0; i < num_threads; i++) {
        other_threads[i] = thread_spawn_joinable("ch", chaos, NULL);
        TEST_ASSERT(other_threads[i]);
    }

    for (size_t i = 0; i < num_threads; i++)
        thread_join(other_threads[i]);

    TEST_ASSERT(atomic_load(&chaos_left) == 0);

    return TEST_SUCCESS;
}
#endif

#ifdef TEST_RWLOCK

#define RWLOCK_REPORT_PROBLEMS()                                               \
    test_info("rwlock tests are encountering problems and will be skipped");   \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct rwlock rw_basic = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

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

#ifdef TEST_SYNC_NIGHTMARE
/* TODO: We must migrate this to nightmare test framework sometime */
TEST_GROUP_DECLARE(sync_nightmare);

#define CHAOS_THREADS 12
static size_t chaos_iters_count = 200;

#if 0
#define CHAOS_LOG(fmt, ...)                                                    \
    test_info("[chaos %lu] " fmt, time_get_ms(), ##__VA_ARGS__)
#else
#define CHAOS_LOG(fmt, ...) ((void) 0)
#endif

struct chaos_state {
    struct thread *t;
    atomic_bool alive;
};

static struct chaos_state states[CHAOS_THREADS];
static atomic_bool chaos_stop = false;
static atomic_bool starter_ok = false;
static _Atomic uint32_t sync_chaos_left = CHAOS_THREADS;

static void chaos_apc_fn(void *arg) {
    (void) arg;
    CHAOS_LOG("apc executed on %p", thread_get_current());
}

static void chaos_apc_spammer() {
    CHAOS_LOG("apc spammer start");

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % CHAOS_THREADS;

        if (!atomic_load(&states[id].alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        CHAOS_LOG("queue apc to %p", states[id].t);
        apc_queue(apc_create(chaos_apc_fn, NULL), states[id].t);
        thread_put(states[id].t);

        scheduler_yield();
    }

    CHAOS_LOG("apc spammer exit");
}

static void chaos_sleeper(void *arg) {
    size_t id = (size_t) arg;

    while (!atomic_load(&starter_ok))
        scheduler_yield();

    CHAOS_LOG("sleeper %zu start", id);

    for (size_t i = 0; i < chaos_iters_count; i++) {
        if (atomic_load(&chaos_stop))
            break;

        time_ms_t ms = (prng_next() % 5) + 1;
        uint64_t target = time_get_ms() + ms;

        CHAOS_LOG("sleeper %zu sleep %lu ms", id, ms);

        enum thread_sleep_result res = thread_sleep_interruptible(target);

        CHAOS_LOG("sleeper %zu woke (%s)", id,
                  res == THREAD_SLEEP_RESULT_TIMEOUT ? "TIMEOUT" : "INTERRUPTED");

        scheduler_yield();
    }

    CHAOS_LOG("sleeper %zu exit", id);
    atomic_store(&states[id].alive, false);

    if (atomic_fetch_sub(&sync_chaos_left, 1) == 1) {
        CHAOS_LOG("all sleepers done -> stopping chaos");
        atomic_store(&chaos_stop, true);
    }
}

static void chaos_waker() {
    CHAOS_LOG("waker start");

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % CHAOS_THREADS;

        if (!atomic_load(&states[id].alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        CHAOS_LOG("wake %p", states[id].t);
        thread_wake(states[id].t);
        CHAOS_LOG("wake %p ok", states[id].t);
        thread_put(states[id].t);

        scheduler_yield();
    }

    CHAOS_LOG("waker exit");
}

static void chaos_migrator() {
    uint32_t cores = global.core_count;

    CHAOS_LOG("migrator start (%u cores)", cores);

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % CHAOS_THREADS;
        uint32_t core = prng_next() % cores;

        if (!atomic_load(&states[id].alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        CHAOS_LOG("migrate %p to %u", states[id].t, core);
        thread_migrate(states[id].t, core);
        CHAOS_LOG("migrate %p ok", states[id].t);
        thread_put(states[id].t);

        scheduler_yield();
    }

    CHAOS_LOG("migrator exit");
}

TEST_DECLARE_INTEGRATION(thread_interruptible_chaos_fuzz,
                         .group = TEST_GROUP(sync_nightmare),
                         TEST_INTENSITY(20, 200, 1000)) {
    if (global.core_count < 6) {
        test_info("needs 6+ cores for chaos fuzz");
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    chaos_iters_count = ctx->intensity_val ? ctx->intensity_val : 200;

    CHAOS_LOG("chaos test start");

    atomic_store(&chaos_stop, false);
    atomic_store(&starter_ok, false);
    atomic_store(&sync_chaos_left, CHAOS_THREADS);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        atomic_store(&states[i].alive, true);
        states[i].t = thread_spawn_joinable_on_core(
            "chaos_sleeper", chaos_sleeper, (void *) i, i % global.core_count);

        if (!states[i].t && atomic_fetch_sub(&sync_chaos_left, 1) == 1)
            atomic_store(&chaos_stop, true);
    }

    atomic_store(&starter_ok, true);

    struct thread *waker =
        thread_spawn_joinable("chaos_wake", chaos_waker, NULL);
    struct thread *migrator =
        thread_spawn_joinable("chaos_migrate", chaos_migrator, NULL);
    struct thread *spammer =
        thread_spawn_joinable("chaos_apc", chaos_apc_spammer, NULL);
    irql_lower(irql);

    if (waker)
        thread_join(waker);

    if (migrator)
        thread_join(migrator);

    if (spammer)
        thread_join(spammer);

    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        if (states[i].t)
            thread_join(states[i].t);
    }

    TEST_ASSERT(atomic_load(&sync_chaos_left) == 0);

    CHAOS_LOG("chaos test complete");
    return TEST_SUCCESS;
}
#endif
