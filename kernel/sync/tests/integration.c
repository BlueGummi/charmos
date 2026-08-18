#include "test_internal.h"

#ifdef TEST_MUTEX

#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct mutex basic_test_mtx = MUTEX_INIT;

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

TEST_DECLARE_INTEGRATION(mutex_many_waiters, .group = TEST_GROUP(mutex)) {
    struct thread *workers[MUTEX_MANY_WAITER_TEST_WAITER_COUNT];

    for (int i = 0; i < MUTEX_MANY_WAITER_TEST_WAITER_COUNT; i++) {
        struct thread *t = thread_create("mw", many_worker, NULL);
        TEST_ASSERT(t);

        t->flags |= THREAD_FLAG_PINNED;
        thread_set_joinable(t);
        thread_enqueue(t);
        workers[i] = t;
    }

    for (int i = 0; i < MUTEX_MANY_WAITER_TEST_WAITER_COUNT; i++)
        thread_join(workers[i]);

    TEST_ASSERT(atomic_load(&many_waiter_done) == 0);

    return TEST_SUCCESS;
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
struct thread *other_threads[CHAOS_THREAD_COUNT] = {0};

TEST_DECLARE_INTEGRATION(mutex_chaos, .group = TEST_GROUP(mutex)) {
    main_thread = thread_get_current();
    for (int i = 0; i < CHAOS_THREAD_COUNT; i++) {
        other_threads[i] = thread_spawn_joinable("ch", chaos, NULL);
        TEST_ASSERT(other_threads[i]);
    }

    for (int i = 0; i < CHAOS_THREAD_COUNT; i++)
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

#define RWLOCK_READER_COUNT_TEST_N 20
#define RWLOCK_READER_COUNT_LOOPS 500
#define RWLOCK_READER_PRINT_INTERVAL 10000

static struct rwlock rw_readers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static _Atomic uint32_t rw_readers_left = RWLOCK_READER_COUNT_TEST_N;

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
            rw_lock(&rw_correct, RWLOCK_ACQUIRE_READ);

            atomic_fetch_add(&active_readers, 1);
            if (atomic_load(&active_writers) != 0)
                atomic_store(&correctness_ok, false);

            scheduler_yield();
            atomic_fetch_sub(&active_readers, 1);

            rw_unlock(&rw_correct);
        } else {
            // Writer

            rw_lock(&rw_correct, RWLOCK_ACQUIRE_WRITE);

            atomic_fetch_add(&active_writers, 1);
            if (atomic_load(&active_readers) != 0 ||
                atomic_load(&active_writers) > 1)
                atomic_store(&correctness_ok, false);

            scheduler_yield();
            atomic_fetch_sub(&active_writers, 1);

            rw_unlock(&rw_correct);
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

/* sync_nightmare: self-contained, no build flag in the original */
TEST_GROUP_DECLARE(sync_nightmare);

#define CHAOS_THREADS 16
#define CHAOS_ITERS 50000

struct chaos_thread_state {
    struct thread *t;
    atomic_bool alive;
    atomic_bool ready;
    _Atomic uintptr_t last_cookie;
    enum thread_wake_reason last_reason;
};

static struct chaos_thread_state states[CHAOS_THREADS];
static atomic_bool chaos_stop = false;
static atomic_bool starter_ok = false;
static atomic_uint sync_chaos_left = CHAOS_THREADS;

/* ------------------------------------
 * Rate-limited logging helpers
 * ------------------------------------ */

#define CHAOS_LOG_INTERVAL_NS (50ULL * 1000 * 1000) /* 50ms */
#define CHAOS_LOG_BURST 3

static inline bool chaos_log_allow(uint64_t *last_ns, uint32_t *burst) {
    uint64_t now = time_get_ns();

    if (now - *last_ns > CHAOS_LOG_INTERVAL_NS) {
        *last_ns = now;
        *burst = 0;
        return true;
    }

    if (*burst < CHAOS_LOG_BURST) {
        (*burst)++;
        return true;
    }

    return false;
}

#define CHAOS_LOG(fmt, ...)                                                    \
    do {                                                                       \
        static uint64_t _last_ns;                                              \
        static uint32_t _burst;                                                \
        if (chaos_log_allow(&_last_ns, &_burst))                               \
            printf("[chaos %llu ms] " fmt "\n", time_get_ms(), ##__VA_ARGS__); \
    } while (0)

/* ------------------------------------
 * APC spammer callback
 * ------------------------------------ */
static void chaos_apc_fn(void *apc) {
    (void) apc;
    /* No signal needed; the wake logic handles APC ordering. */
}

/* ------------------------------------
 * Thread: Sleeper
 * Random interruptible sleeps
 * ------------------------------------ */
static void chaos_sleeper(void *arg) {
    size_t id = (size_t) arg;
    struct chaos_thread_state *s = &states[id];

    CHAOS_LOG("sleeper[%zu] start on core %u", id, smp_core_id());

    while (!atomic_load(&starter_ok))
        cpu_relax();

    for (int i = 0; i < CHAOS_ITERS && !atomic_load(&chaos_stop); i++) {
        uintptr_t cookie = prng_next();
        atomic_store(&s->last_cookie, cookie);
        atomic_store(&s->ready, false);

        CHAOS_LOG("sleeper[%zu] sleep iter=%d cookie=%p", id, i,
                  (void *) cookie);
        thread_prepare_to_sleep(thread_get_current(),
                                THREAD_SLEEP_REASON_MANUAL,
                                THREAD_WAIT_INTERRUPTIBLE, (void *) cookie);

        thread_yield_until_wake_match();

        atomic_store(&s->ready, true);

        if (prng_next() % 5 == 0)
            scheduler_yield();
    }

    CHAOS_LOG("sleeper[%zu] exit", id);

    atomic_store(&s->alive, false);

    /* the last sleeper out winds the chaos threads down. that ordering is
     * what lets the test join them before it drops its references to the
     * sleepers, which they poke at through states[].t */
    if (atomic_fetch_sub(&sync_chaos_left, 1) == 1)
        atomic_store(&chaos_stop, true);
}

/* ------------------------------------
 * Thread: Waker
 * Randomly wakes sleeper threads
 * ------------------------------------ */
static void chaos_waker(void *a) {
    (void) a;
    CHAOS_LOG("waker start");

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % CHAOS_THREADS;
        struct chaos_thread_state *s = &states[id];

        if (!atomic_load(&s->alive))
            continue;

        if (!thread_get(s->t))
            continue;

        bool correct = (prng_next() % 3 == 0);
        uintptr_t cookie = correct ? atomic_load(&s->last_cookie) : prng_next();

        CHAOS_LOG("waker wake %p", s->t);
        thread_wake(s->t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                    s->t->perceived_prio_class, (void *) cookie);
        CHAOS_LOG("waker wake done");

        thread_put(s->t);

        if (prng_next() % 2)
            scheduler_yield();
    }

    CHAOS_LOG("waker exit");
}

/* ------------------------------------
 * Thread: APC Spammer
 * ------------------------------------ */
static void chaos_apc_spammer(void *arg) {
    (void) arg;
    static atomic_uint apc_count = 0;

    CHAOS_LOG("apc spammer start");

    while (!atomic_load(&chaos_stop)) {

        int id = prng_next() % CHAOS_THREADS;
        struct chaos_thread_state *s = &states[id];

        if (!atomic_load(&s->alive))
            continue;

        if (!thread_get(s->t))
            continue;

        struct apc *apc = apc_create();
        apc_init(apc, chaos_apc_fn, NULL);
        apc_enqueue(s->t, apc, APC_TYPE_KERNEL);

        uint32_t n = atomic_fetch_add(&apc_count, 1) + 1;
        if ((n & 0xfff) == 0) {
            CHAOS_LOG("apc spammer queued %u APCs", n);
        }

        thread_put(s->t);
        scheduler_yield();
    }

    CHAOS_LOG("apc spammer exit");
}

/* ------------------------------------
 * Thread: Migrator
 * Moves threads across cores
 * ------------------------------------ */
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

/* ------------------------------------
 * Main Test
 * ------------------------------------ */
TEST_DECLARE_INTEGRATION(thread_interruptible_chaos_fuzz,
                         .group = TEST_GROUP(sync_nightmare)) {
    test_info("this test is long. comment me out to run it.");
    return TEST_SKIP(TEST_SKIP_NONE);

    CHAOS_LOG("chaos test start");

    if (global.core_count < 6) {
        test_info("needs 6+ cores for chaos fuzz");
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        atomic_store(&states[i].alive, true);
        states[i].t = thread_spawn_joinable_on_core(
            "chaos_sleeper", chaos_sleeper, (void *) i, i % global.core_count);

        /* a sleeper that never ran still has to be accounted for, or
         * nothing ever sets chaos_stop and the joins below hang */
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

    /* chaos threads first: they exit once the last sleeper has set
     * chaos_stop, and they must be gone before we drop the sleeper
     * references they are dereferencing */
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
