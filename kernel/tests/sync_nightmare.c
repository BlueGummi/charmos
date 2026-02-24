#include <crypto/prng.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <tests.h>
#include <thread/apc.h>
#include <thread/thread.h>

#define CHAOS_THREADS 16
#define CHAOS_ITERS 50000

struct chaos_thread_state {
    struct thread *t;
    atomic_bool alive;
    atomic_bool ready;
    uintptr_t last_cookie;
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
            k_printf("[chaos %llu ms] " fmt "\n", time_get_ms(),               \
                     ##__VA_ARGS__);                                           \
    } while (0)

/* ------------------------------------
 * APC spammer callback
 * ------------------------------------ */
static void chaos_apc_fn(struct apc *apc, void *a, void *b) {
    (void) apc;
    (void) a;
    (void) b;
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
        s->last_cookie = cookie;
        atomic_store(&s->ready, false);

        CHAOS_LOG("sleeper[%zu] sleep iter=%d cookie=0x%lx", id, i,
                  (void *) cookie);
        thread_sleep(thread_get_current(), THREAD_SLEEP_REASON_MANUAL,
                     THREAD_WAIT_INTERRUPTIBLE, (void *) cookie);

        thread_wait_for_wake_match();

        atomic_store(&s->ready, true);

        if (prng_next() % 5 == 0)
            scheduler_yield();
    }

    CHAOS_LOG("sleeper[%zu] exit", id);

    atomic_fetch_sub(&sync_chaos_left, 1);
    atomic_store(&s->alive, false);
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
        uintptr_t cookie = correct ? s->last_cookie : prng_next();

        CHAOS_LOG("waker wake 0x%lx", s->t);
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
        apc_init(apc, chaos_apc_fn, NULL, NULL);
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

        CHAOS_LOG("migrate 0x%lx to %u", states[id].t, core);
        thread_migrate(states[id].t, core);
        CHAOS_LOG("migrate 0x%lx ok", states[id].t);
        thread_put(states[id].t);

        scheduler_yield();
    }

    CHAOS_LOG("migrator exit");
}

/* ------------------------------------
 * Main Test
 * ------------------------------------ */
TEST_REGISTER(thread_interruptible_chaos_fuzz, SHOULD_NOT_FAIL,
              IS_INTEGRATION_TEST) {
    ADD_MESSAGE("this test is long. comment me out to run it.");
    SET_SKIP();
    return;

    CHAOS_LOG("chaos test start");

    if (global.core_count < 6) {
        ADD_MESSAGE("needs 6+ cores for chaos fuzz");
        SET_SKIP();
        return;
    }

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        atomic_store(&states[i].alive, true);
        states[i].t = thread_spawn_on_core("chaos_sleeper", chaos_sleeper,
                                           (void *) i, i % global.core_count);
    }

    atomic_store(&starter_ok, true);

    thread_spawn("chaos_wake", chaos_waker, NULL);
    thread_spawn("chaos_migrate", chaos_migrator, NULL);
    thread_spawn("chaos_apc", chaos_apc_spammer, NULL);
    irql_lower(irql);

    uint64_t last_report = time_get_ms();

    while (atomic_load(&sync_chaos_left)) {
        if (time_get_ms() - last_report > 1000) {
            last_report = time_get_ms();
            CHAOS_LOG("waiting: %u sleepers left",
                      atomic_load(&sync_chaos_left));
        }
        scheduler_yield();
    }

    atomic_store(&chaos_stop, true);

    CHAOS_LOG("chaos test complete");
    SET_SUCCESS();
}
