#include "sync/tests/test_internal.h"

#if defined(TEST_SYNC_NIGHTMARE) || defined(TEST_MUTEX)

struct chaos_state {
    struct thread *t;
    atomic_bool alive;
};

#define CHAOS_THREADS_MAX 64
static size_t chaos_threads = 12;

#if 0
#define CHAOS_LOG(fmt, ...)                                                    \
    test_info("[chaos %lu] " fmt, time_get_ms(), ##__VA_ARGS__)
#else
#define CHAOS_LOG(fmt, ...) ((void) 0)
#endif

static size_t chaos_iters_count = 300;
static struct chaos_state states[CHAOS_THREADS_MAX];
static atomic_bool chaos_stop = false;
static atomic_bool starter_ok = false;
static _Atomic uint32_t sync_chaos_left = 0;

static struct mutex chaos_fuzz_mtx = MUTEX_INIT;
static struct rwlock chaos_fuzz_rw = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static struct spinlock chaos_fuzz_spin = SPINLOCK_INIT;
static struct qspinlock chaos_fuzz_qspin = QSPINLOCK_INIT;

static _Atomic uint64_t chaos_apc_lock_taken = 0;
static _Atomic uint64_t chaos_apc_lock_skips = 0;

/* Contend on a global lock from inside the APC,
 * as the interleaving is what we fuzz.
 *
 * Blocking here means we spin irq disabled,
 * so we trylock and record skips */
static void chaos_apc_fn(void *arg) {
    unused(arg);
    CHAOS_LOG("apc executed on %p", thread_get_current());

    enum irql irql;
    if (!spin_trylock_irq_disable(&chaos_fuzz_spin, &irql)) {
        atomic_fetch_add_explicit(&chaos_apc_lock_skips, 1,
                                  memory_order_relaxed);
        return;
    }

    atomic_fetch_add_explicit(&chaos_apc_lock_taken, 1, memory_order_relaxed);
    for (volatile int j = 0; j < 10; j++)
        cpu_relax();
    spin_unlock(&chaos_fuzz_spin, irql);
}

static void chaos_apc_spammer(void *arg) {
    unused(arg);
    CHAOS_LOG("apc spammer start");

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % chaos_threads;

        if (!atomic_load(&states[id].alive)) {
            scheduler_yield();
            continue;
        }

        if (!thread_get(states[id].t)) {
            scheduler_yield();
            continue;
        }

        struct apc *a = kmalloc(sizeof(struct apc), ALLOC_FLAGS_ZERO);
        if (a) {
            apc_init(a, chaos_apc_fn, NULL, apc_destroy_free);
            CHAOS_LOG("queue apc to %p", states[id].t);
            apc_enqueue(states[id].t, a, APC_TYPE_KERNEL);
            apc_put(a);
        }
        thread_put(states[id].t);

        scheduler_yield();
    }
}

static void chaos_sleeper(void *arg) {
    size_t id = (size_t) arg;
    struct thread *t = thread_get_current();
    states[id].t = t;
    atomic_store(&states[id].alive, true);

    while (!atomic_load(&starter_ok))
        cpu_relax();

    for (size_t i = 0; i < chaos_iters_count; i++) {
        /* Exercise mutex */
        mutex_lock(&chaos_fuzz_mtx);
        for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
            cpu_relax();
        mutex_unlock(&chaos_fuzz_mtx);

        /* Exercise rwlock */
        if (prng_next() & 1) {
            rw_lock(&chaos_fuzz_rw, RWLOCK_ACQUIRE_READ);
            for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
                cpu_relax();
            rw_unlock(&chaos_fuzz_rw);
        } else {
            rw_lock(&chaos_fuzz_rw, RWLOCK_ACQUIRE_WRITE);
            for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
                cpu_relax();
            rw_unlock(&chaos_fuzz_rw);
        }

        /* Exercise qspinlock */
        enum irql irql = qspin_lock(&chaos_fuzz_qspin);
        for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
            cpu_relax();
        qspin_unlock(&chaos_fuzz_qspin, irql);

        /* Sleep and wait for waker */
        thread_prepare_to_sleep(t, THREAD_SLEEP_REASON_MANUAL,
                                THREAD_WAIT_INTERRUPTIBLE, (void *) id);
        thread_yield_until_wake_match();
        CHAOS_LOG("sleeper %zu woke up, iter %zu", id, i);
    }

    atomic_store(&states[id].alive, false);
    atomic_fetch_sub(&sync_chaos_left, 1);
}

static void chaos_waker(void *arg) {
    unused(arg);
    CHAOS_LOG("waker start");

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % chaos_threads;

        if (!atomic_load(&states[id].alive)) {
            scheduler_yield();
            continue;
        }

        if (!thread_get(states[id].t)) {
            scheduler_yield();
            continue;
        }

        CHAOS_LOG("wake %p", states[id].t);
        thread_wake(states[id].t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                    THREAD_PRIO_CLASS_TIMESHARE, (void *) (uintptr_t) id);
        thread_put(states[id].t);

        scheduler_yield();
    }
}

TEST_DECLARE_INTEGRATION(mutex, interruptible_apc_fuzz,
                         TEST_INTENSITY(4, 12, CHAOS_THREADS_MAX)) {
    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    chaos_threads = ctx->intensity_val ? ctx->intensity_val : 12;
    if (chaos_threads > CHAOS_THREADS_MAX)
        chaos_threads = CHAOS_THREADS_MAX;

    for (size_t i = 0; i < chaos_threads; i++) {
        states[i].t = NULL;
        atomic_store(&states[i].alive, false);
    }

    atomic_store(&sync_chaos_left, chaos_threads);
    atomic_store(&chaos_stop, false);
    atomic_store(&starter_ok, false);
    atomic_store(&chaos_apc_lock_taken, 0);
    atomic_store(&chaos_apc_lock_skips, 0);

    struct thread *threads[CHAOS_THREADS_MAX];
    for (size_t i = 0; i < chaos_threads; i++) {
        threads[i] = thread_create("cs", chaos_sleeper, (void *) i);
        TEST_ASSERT_NONNULL(threads[i]);
        thread_set_joinable(threads[i]);
        thread_enqueue(threads[i]);
    }

    struct thread *spammer =
        thread_spawn_joinable("chaos_apc_spammer", chaos_apc_spammer, NULL);
    TEST_ASSERT_NONNULL(spammer);
    struct thread *waker =
        thread_spawn_joinable("chaos_waker", chaos_waker, NULL);
    TEST_ASSERT_NONNULL(waker);

    atomic_store(&starter_ok, true);

    for (size_t i = 0; i < chaos_threads; i++)
        thread_join(threads[i]);

    atomic_store(&chaos_stop, true);

    thread_join(spammer);
    thread_join(waker);

    TEST_ASSERT_EQ(atomic_load(&sync_chaos_left), 0);

    test_info("apc lock: %llu taken, %llu skipped",
              (unsigned long long) atomic_load(&chaos_apc_lock_taken),
              (unsigned long long) atomic_load(&chaos_apc_lock_skips));

    return TEST_SUCCESS;
}

#endif /* defined(TEST_SYNC_NIGHTMARE) || defined(TEST_MUTEX) */
