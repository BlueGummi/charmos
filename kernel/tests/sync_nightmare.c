#include <crypto/prng.h>
#include <mem/alloc.h>
#include <sch/apc.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <tests.h>

#define CHAOS_THREADS 6
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
    while (!atomic_load(&starter_ok))
        cpu_relax();

    struct chaos_thread_state *s = &states[(size_t) arg];

    for (int i = 0; i < CHAOS_ITERS && !atomic_load(&chaos_stop); i++) {

        uintptr_t cookie = prng_next();
        s->last_cookie = cookie;
        atomic_store(&s->ready, false);

        thread_sleep(scheduler_get_current_thread(), THREAD_SLEEP_REASON_MANUAL,
                     THREAD_WAIT_INTERRUPTIBLE, (void *) cookie);

        /* Let it block */
        thread_wait_for_wake_match();

        /* Mark that a valid wake happened */
        atomic_store(&s->ready, true);

        /* Re-schedule aggressively */
        if (prng_next() % 5 == 0)
            scheduler_yield();
    }

    atomic_store(&s->alive, false);
}

/* ------------------------------------
 * Thread: Waker
 * Randomly wakes sleeper threads
 * ------------------------------------ */
static void chaos_waker() {
    while (!atomic_load(&chaos_stop)) {

        int id = prng_next() % CHAOS_THREADS;
        struct chaos_thread_state *s = &states[id];
        if (!atomic_load(&s->alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        uintptr_t maybe_cookie =
            (prng_next() % 3 == 0) ? s->last_cookie : prng_next();

        scheduler_wake(s->t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                       s->t->perceived_prio_class, (void *) maybe_cookie);

        thread_put(states[id].t);

        if (prng_next() % 2)
            scheduler_yield();
    }
}

/* ------------------------------------
 * Thread: APC Spammer
 * ------------------------------------ */
static void chaos_apc_spammer() {
    while (!atomic_load(&chaos_stop)) {

        int id = prng_next() % CHAOS_THREADS;
        struct chaos_thread_state *s = &states[id];
        if (!atomic_load(&s->alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        struct apc *apc = apc_create();
        apc_init(apc, chaos_apc_fn, NULL, NULL);
        apc_enqueue(s->t, apc, APC_TYPE_KERNEL);

        thread_put(states[id].t);

        scheduler_yield();
    }
}

/* ------------------------------------
 * Thread: Migrator
 * Moves threads across cores
 * ------------------------------------ */
static void chaos_migrator() {

    uint32_t cores = global.core_count;

    while (!atomic_load(&chaos_stop)) {
        int id = prng_next() % CHAOS_THREADS;

        uint32_t core = prng_next() % cores;

        if (!atomic_load(&states[id].alive))
            continue;

        if (!thread_get(states[id].t))
            continue;

        thread_migrate(states[id].t, core);
        thread_put(states[id].t);

        scheduler_yield();
    }
}

/* ------------------------------------
 * Main Test
 * ------------------------------------ */
REGISTER_TEST(thread_interruptible_chaos_fuzz, SHOULD_NOT_FAIL,
              IS_INTEGRATION_TEST) {
    ADD_MESSAGE("This test takes a long time and is off by default. "
                "Comment out these messages to run it.");
    SET_SKIP();
    return;

    if (global.core_count < 6) {
        ADD_MESSAGE("needs 6+ cores for chaos fuzz");
        SET_SKIP();
        return;
    }

    /* Spawn chaos sleeper threads */
    for (size_t i = 0; i < CHAOS_THREADS; i++) {
        atomic_store(&states[i].alive, true);
        states[i].t = thread_spawn_on_core("chaos_sleeper", chaos_sleeper,
                                           (void *) i, i % global.core_count);
    }

    atomic_store(&starter_ok, true);

    /* Spawn chaos components */
    thread_spawn("chaos_wake", chaos_waker, NULL);
    thread_spawn("chaos_apc", chaos_apc_spammer, NULL);
    thread_spawn("chaos_migrate", chaos_migrator, NULL);

    /* Wait for all sleepers to complete */
    bool all_done = false;
    while (!all_done) {
        all_done = true;
        for (int i = 0; i < CHAOS_THREADS; i++) {
            if (atomic_load(&states[i].alive)) {
                all_done = false;
                break;
            }
        }
        scheduler_yield();
    }

    atomic_store(&chaos_stop, true);

    SET_SUCCESS();
}
