#include "sync/tests/test_internal.h"
#include <mem/nofail.h>

struct chaos_state {
    struct thread_wait_header wait;
    struct spinlock lock;
    bool signaled;
    struct thread *t;
    atomic_bool alive;
};

#define CHAOS_THREADS_MAX 64

#define CHAOS_THREADS_MIN 2
#define CHAOS_THREADS_DEFAULT 8
#define CHAOS_THREADS_INTENSE 32

static size_t chaos_threads = CHAOS_THREADS_DEFAULT;

#if 0
#define CHAOS_LOG(fmt, ...)                                                    \
    test_info("[chaos %lu] " fmt, time_get_ms(), ##__VA_ARGS__)
#else
#define CHAOS_LOG(fmt, ...) ((void) 0)
#endif

#define CHAOS_ITERS_DEFAULT 200
#define CHAOS_ITERS_MIN 16

#define CHAOS_APC_PERIOD_BASE 2

static size_t chaos_iters_count = CHAOS_ITERS_DEFAULT;
static size_t chaos_apc_period = CHAOS_APC_PERIOD_BASE;
static struct chaos_state states[CHAOS_THREADS_MAX];
static atomic_bool chaos_stop = false;
static atomic_bool starter_ok = false;
static atomic_uint32_t sync_chaos_left = 0;

static struct mutex chaos_fuzz_mtx = MUTEX_INIT;
static struct rwlock chaos_fuzz_rw = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static struct spinlock chaos_fuzz_spin = SPINLOCK_INIT;
static struct qspinlock chaos_fuzz_qspin = QSPINLOCK_INIT;

static atomic_uint64_t chaos_apc_lock_taken = 0;
static atomic_uint64_t chaos_apc_lock_skips = 0;

static atomic_uint64_t chaos_iters[CHAOS_THREADS_MAX];
static atomic_uint64_t chaos_spammer_iters;
static atomic_uint64_t chaos_waker_iters;

#define CHAOS_DIAG_SPAMMER CHAOS_THREADS_MAX
#define CHAOS_DIAG_WAKER (CHAOS_THREADS_MAX + 1)
#define CHAOS_DIAG_COUNT (CHAOS_THREADS_MAX + 2)

static struct thread_diag *chaos_diag[CHAOS_DIAG_COUNT];

static void chaos_diag_attach(struct thread *t, size_t slot) {
    if (!t || chaos_diag[slot])
        return;

    struct thread_diag *d =
        nofail_valloc(sizeof(struct thread_diag), ALLOC_ZERO);

    if (!thread_diag_attach(t, d)) {
        vfree(d);
        return;
    }

    chaos_diag[slot] = d;
}

static void chaos_diag_release(struct thread *t, size_t slot) {
    if (t)
        thread_diag_detach(t);

    vfree(chaos_diag[slot]);
    chaos_diag[slot] = NULL;
}

static void chaos_apc_fn(void *arg) {
    cc_unused(arg);
    CHAOS_LOG("apc executed on %p", thread_get_current());

    enum irql irql;
    if (!spin_trylock_high(&chaos_fuzz_spin, &irql)) {
        atomic_inc_rlx(&chaos_apc_lock_skips);
        return;
    }

    atomic_inc_rlx(&chaos_apc_lock_taken);
    for (volatile int j = 0; j < 10; j++)
        cpu_pause();
    spin_unlock(&chaos_fuzz_spin, irql);
}

static void chaos_apc_spammer(void *arg) {
    cc_unused(arg);
    CHAOS_LOG("apc spammer start");

    size_t pass = 0;

    while (!atomic_load(&chaos_stop)) {
        atomic_inc_rlx(&chaos_spammer_iters);

        if (pass++ % chaos_apc_period) {
            thread_sleep_for_ms(10);
            scheduler_yield();
            continue;
        }

        int id = prng_next() % chaos_threads;

        if (!atomic_load(&states[id].alive)) {
            thread_sleep_for_ms(10);
            scheduler_yield();
            continue;
        }

        rcu_read_lock();
        bool got = thread_get_rcu(states[id].t);
        rcu_read_unlock();

        if (!got) {
            thread_sleep_for_ms(10);
            scheduler_yield();
            continue;
        }

        struct apc *a = valloc(sizeof(struct apc), ALLOC_ZERO);
        if (a) {
            apc_init(a, chaos_apc_fn, NULL, apc_destroy_free);
            CHAOS_LOG("queue apc to %p", states[id].t);
            apc_enqueue(states[id].t, a, APC_TYPE_KERNEL);
            apc_put(a);
        }
        thread_put(states[id].t);

        thread_sleep_for_ms(10);
        scheduler_yield();
    }
}

static void chaos_sleeper(void *arg) {
    size_t id = (size_t) arg;
    struct thread *t = thread_get_current();
    states[id].t = t;
    atomic_store(&states[id].alive, true);

    while (!atomic_load(&starter_ok))
        cpu_pause();

    for (size_t i = 0; i < chaos_iters_count; i++) {
        atomic_inc_rlx(&chaos_iters[id]);

        /* Exercise mutex */
        mutex_lock(&chaos_fuzz_mtx);
        for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
            cpu_pause();
        mutex_unlock(&chaos_fuzz_mtx);

        /* Exercise rwlock */
        if (prng_next() & 1) {
            rw_lock(&chaos_fuzz_rw, RWLOCK_READ);
            for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
                cpu_pause();
            rw_unlock(&chaos_fuzz_rw);
        } else {
            rw_lock(&chaos_fuzz_rw, RWLOCK_WRITE);
            for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
                cpu_pause();
            rw_unlock(&chaos_fuzz_rw);
        }

        /* Exercise qspinlock */
        enum irql irql = qspin_lock(&chaos_fuzz_qspin);
        for (volatile int j = 0; j < (int) (prng_next() & 0xF); j++)
            cpu_pause();
        qspin_unlock(&chaos_fuzz_qspin, irql);

        /* Sleep and wait for waker */
        struct chaos_state *state = &states[id];
        irql = spin_lock_high(&state->lock);
        if (!state->signaled) {
            thread_wait_prepare_to_sleep(&state->wait, state,
                                         THREAD_WAIT_INTERRUPTIBLE);
            spin_unlock(&state->lock, irql);
            thread_wait_complete();
            irql = spin_lock_high(&state->lock);
        }
        state->signaled = false;
        spin_unlock(&state->lock, irql);
        CHAOS_LOG("sleeper %zu woke up, iter %zu", id, i);
    }

    atomic_store(&states[id].alive, false);
    atomic_dec(&sync_chaos_left);
}

static void chaos_waker(void *arg) {
    cc_unused(arg);
    CHAOS_LOG("waker start");

    while (!atomic_load(&chaos_stop)) {
        atomic_inc_rlx(&chaos_waker_iters);
        int id = prng_next() % chaos_threads;

        if (!atomic_load(&states[id].alive)) {
            scheduler_yield();
            continue;
        }

        rcu_read_lock();
        bool got = thread_get_rcu(states[id].t);
        rcu_read_unlock();

        if (!got) {
            scheduler_yield();
            continue;
        }

        CHAOS_LOG("wake %p", states[id].t);
        struct chaos_state *state = &states[id];
        enum irql irql = spin_lock_high(&state->lock);
        state->signaled = true;
        thread_wait_header_satisfy(&state->wait,
                                   THREAD_WAKE_REASON_SLEEP_MANUAL, NULL);
        spin_unlock(&state->lock, irql);
        thread_put(states[id].t);

        scheduler_yield();
    }
}

/* Ring depth per thread in a stall report */
#define CHAOS_ARMS_FOCUS 16
#define CHAOS_ARMS_LIVE 4

#define CHAOS_JOIN_POLL_MS 5000
#define CHAOS_QUIET_POLLS 3
#define CHAOS_MAX_REPORTS 2

static bool chaos_progress_moved(atomic_uint64_t *counter, uint64_t *last) {
    uint64_t now = atomic_load_rlx(counter);
    bool moved = now != *last;
    *last = now;
    return moved;
}

/* `max_arms` caps the wait-arm ring dump and 0 skips the trace */
static void chaos_dump_thread(const char *role, size_t idx, struct thread *t,
                              uint64_t max_arms) {
    if (!t) {
        log_msg(LOG_ERROR, "  %s[%zu]: <null>", role, idx);
        return;
    }

    log_msg(LOG_ERROR, "  %s[%zu] '%s': state=%d flags=0x%x wait_type=%d", role,
            idx, t->name, (int) thread_get_state(t),
            (unsigned) thread_get_flags(t), (int) thread_get_wait_type(t));

    log_msg(LOG_ERROR,
            "  %s[%zu]   mask=0x%x apcs_ran=%u "
            "ctxsw=%zu",
            role, idx, (unsigned) atomic_load(&t->apc_pending_mask),
            t->total_apcs_ran, t->context_switches);

    if (max_arms)
        thread_dump_wait_trace(t, role, idx, max_arms);
}

static void chaos_report_stall(const char *waiting_on, size_t waiting_idx,
                               struct thread **threads, struct thread *spammer,
                               struct thread *waker) {
    log_msg(LOG_ERROR,
            "chaos: no progress while joining %s[%zu] -- left=%u stop=%d "
            "threads=%zu apc_taken=%llu apc_skips=%llu",
            waiting_on, waiting_idx, atomic_load(&sync_chaos_left),
            (int) atomic_load(&chaos_stop), chaos_threads,
            (unsigned long long) atomic_load(&chaos_apc_lock_taken),
            (unsigned long long) atomic_load(&chaos_apc_lock_skips));

    log_msg(LOG_ERROR, "  spammer_iters=%llu waker_iters=%llu",
            (unsigned long long) atomic_load(&chaos_spammer_iters),
            (unsigned long long) atomic_load(&chaos_waker_iters));

    bool stuck_sleeper = strcmp(waiting_on, "sleeper") == 0;

    for (size_t i = 0; i < chaos_threads; i++) {
        log_msg(LOG_ERROR, "  sleeper[%zu] alive=%d iters=%llu", i,
                (int) atomic_load(&states[i].alive),
                (unsigned long long) atomic_load(&chaos_iters[i]));
        uint64_t arms = 0;
        if (stuck_sleeper && i == waiting_idx)
            arms = CHAOS_ARMS_FOCUS;
        else if (atomic_load(&states[i].alive))
            arms = CHAOS_ARMS_LIVE;

        chaos_dump_thread("sleeper", i, threads[i], arms);
    }

    uint64_t helper_arms = stuck_sleeper ? CHAOS_ARMS_LIVE : CHAOS_ARMS_FOCUS;
    chaos_dump_thread("spammer", 0, spammer, helper_arms);
    chaos_dump_thread("waker", 0, waker, helper_arms);
}

static bool chaos_join_watched(struct thread *t, const char *role, size_t idx,
                               atomic_uint64_t *counter,
                               struct thread **threads, struct thread *spammer,
                               struct thread *waker) {
    uint64_t last = atomic_load_rlx(counter);

    size_t quiet = 0;
    size_t reports = 0;
    int status;

    while (!thread_join_timeout(t, CHAOS_JOIN_POLL_MS, &status)) {
        if (chaos_progress_moved(counter, &last)) {
            quiet = 0;
            continue;
        }

        if (++quiet < CHAOS_QUIET_POLLS)
            continue;

        quiet = 0;
        chaos_report_stall(role, idx, threads, spammer, waker);

        if (++reports < CHAOS_MAX_REPORTS)
            continue;

        log_msg(LOG_ERROR,
                "chaos: giving up on %s[%zu] after %zu stall report(s) -- "
                "failing the test",
                role, idx, reports);
        return false;
    }

    return true;
}

TEST_DEFINE_INTEGRATION(mutex, interruptible_apc_fuzz,
                        TEST_INTENSITY(CHAOS_THREADS_MIN, CHAOS_THREADS_DEFAULT,
                                       CHAOS_THREADS_INTENSE),
                        .min_cores = 2) {
    chaos_threads =
        ctx->intensity_val ? ctx->intensity_val : CHAOS_THREADS_DEFAULT;
    if (chaos_threads > CHAOS_THREADS_MAX)
        chaos_threads = CHAOS_THREADS_MAX;

    chaos_iters_count =
        CHAOS_ITERS_DEFAULT * chaos_threads / CHAOS_THREADS_DEFAULT;
    if (chaos_iters_count < CHAOS_ITERS_MIN)
        chaos_iters_count = CHAOS_ITERS_MIN;

    chaos_apc_period =
        CHAOS_APC_PERIOD_BASE * CHAOS_THREADS_DEFAULT / chaos_threads;
    if (!chaos_apc_period)
        chaos_apc_period = 1;

    for (size_t i = 0; i < CHAOS_THREADS_MAX; i++) {
        states[i].t = NULL;
        thread_wait_header_init(&states[i].wait);
        spinlock_init(&states[i].lock);
        states[i].signaled = false;
        atomic_store(&states[i].alive, false);
    }

    atomic_store(&sync_chaos_left, chaos_threads);
    atomic_store(&chaos_stop, false);
    atomic_store(&starter_ok, false);
    atomic_store(&chaos_apc_lock_taken, 0);
    atomic_store(&chaos_apc_lock_skips, 0);
    atomic_store(&chaos_spammer_iters, 0);
    atomic_store(&chaos_waker_iters, 0);
    for (size_t i = 0; i < CHAOS_THREADS_MAX; i++)
        atomic_store(&chaos_iters[i], 0);

    struct thread *threads[CHAOS_THREADS_MAX];
    for (size_t i = 0; i < chaos_threads; i++) {
        threads[i] = thread_create("cs", chaos_sleeper, .arg = (void *) i,
                                   .joinable = true);
        TEST_ASSERT_NONNULL(threads[i]);
        enforce(thread_get(threads[i]));
        chaos_diag_attach(threads[i], i);
        thread_enqueue(threads[i]);
    }

    struct thread *spammer =
        thread_spawn("chaos_apc_spammer", chaos_apc_spammer, .joinable = true);
    TEST_ASSERT_NONNULL(spammer);
    struct thread *waker =
        thread_spawn("chaos_waker", chaos_waker, .joinable = true);
    TEST_ASSERT_NONNULL(waker);

    chaos_diag_attach(spammer, CHAOS_DIAG_SPAMMER);
    chaos_diag_attach(waker, CHAOS_DIAG_WAKER);

    atomic_store(&starter_ok, true);

    bool wedged = false;
    for (size_t i = 0; i < chaos_threads; i++) {
        if (!chaos_join_watched(threads[i], "sleeper", i, &chaos_iters[i],
                                threads, spammer, waker)) {
            wedged = true;
            break;
        }
    }

    atomic_store(&chaos_stop, true);

    if (!wedged) {
        wedged =
            !chaos_join_watched(spammer, "spammer", 0, &chaos_spammer_iters,
                                threads, spammer, waker);
        if (!wedged) {
            /* The join consumed its last caller-owned ref */
            spammer = NULL;
            wedged = !chaos_join_watched(waker, "waker", 0, &chaos_waker_iters,
                                         threads, spammer, waker);
            if (!wedged)
                waker = NULL; /* do not touch it past this point. */
        }
    }

    if (wedged)
        return TEST_FAIL("chaos threads made no progress");

    for (size_t i = 0; i < chaos_threads; i++) {
        chaos_diag_release(threads[i], i);
        thread_put(threads[i]);
    }

    chaos_diag_release(spammer, CHAOS_DIAG_SPAMMER);
    chaos_diag_release(waker, CHAOS_DIAG_WAKER);

    TEST_ASSERT_EQ(atomic_load(&sync_chaos_left), 0);

    test_info("apc lock: %llu taken, %llu skipped",
              (unsigned long long) atomic_load(&chaos_apc_lock_taken),
              (unsigned long long) atomic_load(&chaos_apc_lock_skips));

    return TEST_SUCCESS;
}
