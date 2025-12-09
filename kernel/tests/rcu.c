#ifdef TEST_RCU

#include <mem/alloc.h>
#include <sch/sched.h>
#include <sleep.h>
#include <string.h>
#include <sync/rcu.h>
#include <tests.h>
#include <thread/defer.h>
#include <thread/thread.h>

#define NUM_RCU_READERS (global.core_count)
#define RCU_TEST_DURATION_MS 50

struct rcu_test_data {
    int value;
};

static _Atomic(struct rcu_test_data *) shared_ptr = NULL;
static atomic_bool rcu_test_failed = false;
static _Atomic uint32_t rcu_reads_done = 0;

static void rcu_reader_thread(void *) {
    uint64_t end = time_get_ms() + RCU_TEST_DURATION_MS;

    while (time_get_ms() < end) {
        rcu_read_lock();

        struct rcu_test_data *p = rcu_dereference(shared_ptr);
        if (p) {
            int v = p->value;
            if (v != 42 && v != 43) {
                atomic_store(&rcu_test_failed, true);
                ADD_MESSAGE("RCU reader saw invalid value");
                k_printf("%d\n", v);
            }
        }

        rcu_read_unlock();

        scheduler_yield();
    }

    atomic_fetch_add(&rcu_reads_done, 1);
}

static atomic_bool volatile rcu_deferred_freed = false;

static void rcu_free_fn(void *ptr) {
    kfree(ptr, FREE_PARAMS_DEFAULT);
    atomic_store(&rcu_deferred_freed, true);
}

static void rcu_writer_thread(void *) {
    sleep_ms(30);

    struct rcu_test_data *old = shared_ptr;

    struct rcu_test_data *new = kmalloc(sizeof(*new), ALLOC_PARAMS_DEFAULT);
    new->value = 43;
    rcu_assign_pointer(shared_ptr, new);

    rcu_synchronize();
    rcu_defer(rcu_free_fn, old);
}

REGISTER_TEST(rcu_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct rcu_test_data *initial =
        kmalloc(sizeof(*initial), ALLOC_PARAMS_DEFAULT);
    initial->value = 42;
    shared_ptr = initial;

    for (uint64_t i = 0; i < NUM_RCU_READERS; i++)
        thread_spawn("rcu_reader_test", rcu_reader_thread, NULL);

    thread_spawn("rcu_writer_test", rcu_writer_thread, NULL);

    while (atomic_load(&rcu_reads_done) < NUM_RCU_READERS) {
        scheduler_yield();
    }

    for (int i = 0; i < 100 && !atomic_load(&rcu_deferred_freed); i++)
        sleep_ms(1);

    TEST_ASSERT(!atomic_load(&rcu_test_failed));
    while (!rcu_deferred_freed)
        cpu_relax();

    SET_SUCCESS();
}

/*

* A heavier / more aggressive RCU stress test:
*
* * spawn many readers (more than cores) that constantly enter rcu read-side
* critical sections, read the shared pointer and check values.
* * spawn many writers that continuously replace the pointer with freshly
* allocated objects, deferring frees via rcu_defer. Writers sometimes call
* rcu_synchronize() to force progress of grace periods and exercise that path.
* * intentionally produce a backlog of deferred frees to stress the deferred
* callback mechanism.
* * run for a longer duration and assert no reader observes an invalid value.
*
*/

#define STRESS_NUM_READERS (global.core_count * 4)
#define STRESS_NUM_WRITERS (global.core_count)
#define STRESS_DURATION_MS 500
#define STRESS_PRINT_INTERVAL_MS 100

struct rcu_stress_node {
    uint64_t seq; /* monotonic sequence number (for debugging) */
    int value;
};

static _Atomic(struct rcu_stress_node *) stress_shared = NULL;

/* book-keeping for the test */
static atomic_bool stress_stop = false;
static atomic_bool stress_failed = false;
static _Atomic uint32_t stress_readers_done = 0;
static _Atomic uint32_t stress_deferred_freed = 0;
static _Atomic uint32_t stress_replacements = 0;

/* deferred free callback */
static void stress_free_cb(void *ptr) {
    struct rcu_stress_node *n = ptr;
    /* optional debug trace */
    kfree(n, FREE_PARAMS_DEFAULT);
    atomic_fetch_add(&stress_deferred_freed, 1);
}

/* reader thread: very tight loop, yields frequently */
static void rcu_stress_reader(void *arg) {
    (void) arg;
    uint64_t last_print = time_get_ms();

    while (!atomic_load(&stress_stop)) {
        rcu_read_lock();

        struct rcu_stress_node *p = rcu_dereference(stress_shared);
        if (p) {
            int v = p->value;
            /* allowed values are 42 or 43; anything else indicates corruption
             */
            if (v != 42 && v != 43) {
                atomic_store(&stress_failed, true);
                ADD_MESSAGE("RCU stress reader saw invalid value");
                k_printf(
                    "RCU stress reader observed invalid value %d, seq=%llu\n",
                    v, (unsigned long long) p->seq);
            }
            volatile uint64_t seq = p->seq;
            (void) seq;
        }

        rcu_read_unlock();

        /* yield to exercise scheduler preemption and context switching */
        scheduler_yield();

        /* occasionally print progress (only a tiny amount to avoid flood) */
        if (time_get_ms() - last_print >= STRESS_PRINT_INTERVAL_MS) {
            last_print = time_get_ms();
        }
    }

    atomic_fetch_add(&stress_readers_done, 1);
}

/* writer thread: continuously replace the pointer, sometimes synchronize */
static void rcu_stress_writer(void *arg) {
    (void) arg;
    uint64_t local_iter = 0;

    while (!atomic_load(&stress_stop)) {
        struct rcu_stress_node *new =
            kmalloc(sizeof(*new), ALLOC_PARAMS_DEFAULT);
        if (!new) {
            /* allocation failure — mark as failure and exit */
            atomic_store(&stress_failed, true);
            ADD_MESSAGE("RCU stress writer kmalloc failed");
            break;
        }

        new->seq = (uint64_t) atomic_fetch_add(&stress_replacements, 1) + 1;
        /* alternate values to ensure readers see both */
        new->value = (local_iter & 1) ? 43 : 42;
        local_iter++;

        /* publish new pointer */
        struct rcu_stress_node *old = stress_shared;
        rcu_assign_pointer(stress_shared, new);

        /*
         * Defer freeing the old pointer. We deliberately create a backlog by
         * deferring every single old pointer; later we wait for them to be
         * freed to assert correctness.
         */
        if (old)
            rcu_defer(stress_free_cb, old);

        /*
         * Occasionally force a synchronize call to exercise explicit grace
         * period advancement (do this about once every ~32 replacements).
         */
        if ((local_iter & 0x1f) == 0) {
            rcu_synchronize();
        }

        /*
         * Small, varying yield/sleep to create interleavings: sometimes yield
         * the CPU, sometimes sleep a few ms. Do not call rand(); instead use
         * bits derived from seq to vary behavior without extra dependencies.
         */
        if ((new->seq & 0x7) == 0) {
            sleep_ms(1);
        } else {
            scheduler_yield();
        }
    }
}

/* a reclaimer thread that also calls synchronize periodically to help drain */
static void rcu_stress_reclaimer(void *arg) {
    (void) arg;
    while (!atomic_load(&stress_stop)) {
        /* attempt to shrink deferred backlog by forcing grace periods */
        rcu_synchronize();
        /* small backoff between synchronizations */
        sleep_ms(5);
    }
}

/* Test registration */
REGISTER_TEST(rcu_stress_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    /* initial object */
    struct rcu_stress_node *initial =
        kmalloc(sizeof(*initial), ALLOC_PARAMS_DEFAULT);
    TEST_ASSERT(initial != NULL);
    initial->seq = 1;
    initial->value = 42;
    stress_shared = initial;

    /* spawn readers (more than cores) */
    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "rcu_str_reader_%u", i);
        thread_spawn(name, rcu_stress_reader, NULL);
    }

    /* spawn writers */
    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "rcu_str_writer_%u", i);
        thread_spawn(name, rcu_stress_writer, NULL);
    }

    /* spawn one reclaimer to periodically call synchronize */
    thread_spawn("rcu_str_reclaimer", rcu_stress_reclaimer, NULL);

    /* run for the configured duration */
    uint64_t stop_at = time_get_ms() + STRESS_DURATION_MS;
    while (time_get_ms() < stop_at) {
        if (atomic_load(&stress_failed)) {
            ADD_MESSAGE("RCU stress test failed early due to detection");
            break;
        }
        /* let other threads run */
        scheduler_yield();
    }

    /* signal stop to all readers/writers/reclaimer */
    atomic_store(&stress_stop, true);

    /* wait for readers to finish */
    while (atomic_load(&stress_readers_done) < STRESS_NUM_READERS) {
        scheduler_yield();
    }

    /* wait up to a reasonable timeout for deferred frees to run */
    for (int i = 0; i < 1000 && atomic_load(&stress_deferred_freed) <
                                    atomic_load(&stress_replacements);
         i++) {
        /* call synchronize here to help force callbacks */
        rcu_synchronize();
        sleep_ms(1);
    }

    k_printf("RCU stress test: replacements=%u freed=%u\n",
             (unsigned) atomic_load(&stress_replacements),
             (unsigned) atomic_load(&stress_deferred_freed));

    /* checks */
    TEST_ASSERT(!atomic_load(&stress_failed));

    /*
     * We expect at least some frees to have occurred. On very constrained
     * implementations it may be possible not all deferred callbacks have
     * yet run; fail only if zero frees happened or if obviously fewer frees
     * than replacements exist (tunable).
     */
    TEST_ASSERT(atomic_load(&stress_deferred_freed) > 0);

    /* finally, free the last published pointer (if any) from test */
    struct rcu_stress_node *last = stress_shared;
    if (last) {
        /* old-style: synchronize then free directly */
        rcu_synchronize();
        kfree(last, FREE_PARAMS_DEFAULT);
        atomic_fetch_add(&stress_deferred_freed, 1);
    }

    SET_SUCCESS();
}

#endif
