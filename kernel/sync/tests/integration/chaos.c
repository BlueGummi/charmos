#include "../test_internal.h"

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
}

static void chaos_sleeper(void *arg) {
    size_t id = (size_t) arg;
    struct thread *t = thread_get_current();
    states[id].t = t;
    atomic_store(&states[id].alive, true);

    while (!atomic_load(&starter_ok))
        cpu_relax();

    for (size_t i = 0; i < chaos_iters_count; i++) {
        thread_prepare_to_sleep(t, THREAD_SLEEP_REASON_MANUAL,
                                THREAD_WAIT_INTERRUPTIBLE, (void *) id);
        thread_yield_until_wake_match();
        CHAOS_LOG("sleeper %zu woke up, iter %zu", id, i);
    }

    atomic_store(&states[id].alive, false);
    atomic_fetch_sub(&sync_chaos_left, 1);
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
        thread_wake(states[id].t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                    THREAD_PRIO_CLASS_TIMESHARE, (void *) (uintptr_t) id);
        thread_put(states[id].t);

        scheduler_yield();
    }
}

static void chaos_migrator() {
    CHAOS_LOG("migrator start");

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % CHAOS_THREADS;

        if (!atomic_load(&states[id].alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        size_t target_cpu = prng_next() % global.core_count;
        CHAOS_LOG("migrate %p to cpu %zu", states[id].t, target_cpu);
        scheduler_migrate_thread(states[id].t, target_cpu);
        thread_put(states[id].t);

        scheduler_yield();
    }
}

TEST_DECLARE_INTEGRATION(thread_interruptible_chaos_fuzz,
                         .group = TEST_GROUP(sync_nightmare)) {
    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        states[i].t = NULL;
        atomic_store(&states[i].alive, false);
    }

    atomic_store(&sync_chaos_left, CHAOS_THREADS);
    atomic_store(&chaos_stop, false);
    atomic_store(&starter_ok, false);

    struct thread *threads[CHAOS_THREADS];
    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        threads[i] = thread_create("cs", chaos_sleeper, (void *) i);
        TEST_ASSERT(threads[i]);
        thread_set_joinable(threads[i]);
        thread_enqueue(threads[i]);
    }

    struct thread *spammer =
        thread_spawn("chaos_apc_spammer", chaos_apc_spammer, NULL);
    struct thread *waker = thread_spawn("chaos_waker", chaos_waker, NULL);
    struct thread *migrator =
        thread_spawn("chaos_migrator", chaos_migrator, NULL);

    atomic_store(&starter_ok, true);

    for (size_t i = 0; i < CHAOS_THREADS; i++)
        thread_join(threads[i]);

    atomic_store(&chaos_stop, true);

    thread_detach(spammer);
    thread_detach(waker);
    thread_detach(migrator);

    TEST_ASSERT(atomic_load(&sync_chaos_left) == 0);

    return TEST_SUCCESS;
}

#endif
