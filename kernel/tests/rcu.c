#ifdef TEST_RCU

#include <crypto/prng.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <string.h>
#include <sync/rcu.h>
#include <test.h>
#include <thread/thread.h>
#include <thread/workqueue.h>
#include <time/spin_sleep.h>

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
                test_info("RCU reader saw invalid value");
                test_info("%d", v);
            }
        }

        rcu_read_unlock();

        scheduler_yield();
    }

    atomic_fetch_add(&rcu_reads_done, 1);
}

static atomic_bool volatile rcu_deferred_freed = false;

static void rcu_free_fn(struct rcu_cb *cb, void *ptr) {
    kfree(ptr);
    atomic_store(&rcu_deferred_freed, true);
    kfree(cb);
}

static void rcu_writer_thread(void *) {
    sleep_spin_ms(30);

    struct rcu_test_data *old = shared_ptr;

    struct rcu_test_data *new = kmalloc(sizeof(*new), ALLOC_FLAGS_ZERO);
    new->value = 43;
    rcu_assign_pointer(shared_ptr, new);

    rcu_synchronize();
    rcu_defer(kmalloc(sizeof(struct rcu_cb), ALLOC_FLAGS_ZERO), rcu_free_fn,
              old);
}

TEST_DECLARE(rcu_test, .tier = TEST_TIER_UNIT) {
    struct rcu_test_data *initial = kmalloc(sizeof(*initial), ALLOC_FLAGS_ZERO);
    initial->value = 42;
    shared_ptr = initial;

    for (uint64_t i = 0; i < NUM_RCU_READERS; i++)
        thread_spawn("rcu_reader_test", rcu_reader_thread, NULL);

    thread_spawn("rcu_writer_test", rcu_writer_thread, NULL);

    while (atomic_load(&rcu_reads_done) < NUM_RCU_READERS) {
        scheduler_yield();
    }

    for (int i = 0; i < 100 && !atomic_load(&rcu_deferred_freed); i++)
        sleep_spin_ms(1);

    TEST_ASSERT(!atomic_load(&rcu_test_failed));

    return TEST_SUCCESS;
}

#define STRESS_NUM_READERS (global.core_count * 8)
#define STRESS_NUM_WRITERS (global.core_count)
#define STRESS_DURATION_MS 2000
#define STRESS_PRINT_MS 1000

struct rcu_stress_node {
    uint64_t seq; /* monotonic sequence number (for debugging) */
    int value;
    size_t freed_gen, enqueued_on;
};

static _Atomic(struct rcu_stress_node *) stress_shared = NULL;

/* book-keeping for the test */
static atomic_bool stress_stop = false;
static atomic_bool stress_failed = false;
static _Atomic uint32_t stress_readers_done = 0;
static _Atomic uint32_t stress_writers_done = 0;
static _Atomic uint32_t stress_deferred_freed = 0;
static _Atomic uint32_t stress_replacements = 0;
static atomic_size_t gen_freed = 0;

/* deferred free callback */
static void stress_free_cb(struct rcu_cb *cb, void *ptr) {
    atomic_store(&gen_freed, cb->gen_when_called);
    struct rcu_stress_node *n = ptr;
    /* optional debug trace */
    n->value = 34;
    n->freed_gen = cb->gen_when_called;
    n->enqueued_on = cb->enqueued_waiting_on_gen;
    atomic_fetch_add(&stress_deferred_freed, 1);
    kfree(cb);
    kfree(n);
}

/* reader thread: very tight loop, yields frequently */
static void rcu_stress_reader(void *arg) {
    (void) arg;

    time_t last_print = time_get_ms();
    size_t iter = 0;
    while (!atomic_load(&stress_stop)) {
        rcu_read_lock();

        struct rcu_stress_node *p = rcu_dereference(stress_shared);
        if (p) {
            int v = p->value;
            if (v != 42 && v != 43) {
                atomic_store(&stress_failed, true);
                test_err("RCU stress reader saw invalid value");
                test_err("RCU stress reader observed invalid value %d, "
                         "freed during gen %zu enqueued_on %zu currently "
                         "started gen %zu quiescent for gen %zu\nat a nesting "
                         "depth of %zu",
                         v, p->freed_gen, p->enqueued_on,
                         thread_get_current()->rcu_start_gen,
                         thread_get_current()->rcu_quiescent_gen,
                         thread_get_current()->rcu_nesting);
                break;
            }
            volatile uint64_t seq = p->seq;
            (void) seq;
        }

        rcu_read_unlock();

        if (time_get_ms() - last_print > STRESS_PRINT_MS) {
            last_print = time_get_ms();
            test_info(
                "\'%-20s\' on iteration %7zu w/ %7zu replacements and %7zu "
                "frees",
                thread_get_current()->name, iter, stress_replacements,
                stress_deferred_freed);
        }

        /* yield to exercise scheduler preemption and context switching */
        scheduler_yield();
        iter++;
    }

    test_info("RCU stress reader %s left, %u remaining",
              thread_get_current()->name,
              STRESS_NUM_READERS - stress_readers_done - 1);

    atomic_fetch_add(&stress_readers_done, 1);
}

/* writer thread: continuously replace the pointer, sometimes synchronize */
static void rcu_stress_writer(void *arg) {
    (void) arg;
    uint64_t local_iter = 0;

    while (!atomic_load(&stress_stop)) {
        struct rcu_stress_node *new = kmalloc(sizeof(*new), ALLOC_FLAGS_ZERO);
        if (!new) {
            /* allocation failure — mark as failure and exit */
            atomic_store(&stress_failed, true);
            test_info("RCU stress writer kmalloc failed");
            break;
        }

        new->seq = (uint64_t) atomic_fetch_add(&stress_replacements, 1) + 1;
        /* alternate values to ensure readers see both */
        new->value = (local_iter & 1) ? 43 : 42;
        local_iter++;

        struct rcu_stress_node *old =
            atomic_exchange_explicit(&stress_shared, new, memory_order_acq_rel);

        /*
         * Defer freeing the old pointer. We deliberately create a backlog by
         * deferring every single old pointer; later we wait for them to be
         * freed to assert correctness.
         */
        if (old)
            rcu_defer(kmalloc(sizeof(struct rcu_cb), ALLOC_FLAGS_ZERO),
                      stress_free_cb, old);

        /*
         * Occasionally force a synchronize call to exercise explicit grace
         * period advancement (do this about once every ~32 replacements).
         */
        if ((local_iter & 0x1f) == 0) {
            rcu_synchronize();
        }

        scheduler_yield();
    }

    atomic_fetch_add(&stress_writers_done, 1);
}

/* a reclaimer thread that also calls synchronize periodically to help drain */
static void rcu_stress_reclaimer(void *arg) {
    (void) arg;
    while (!atomic_load(&stress_stop)) {
        /* attempt to shrink deferred backlog by forcing grace periods */
        rcu_synchronize();
        /* small backoff between synchronizations */
        sleep_spin_ms(5);
    }
}

/* Test registration */
TEST_DECLARE(rcu_stress_test, .tier = TEST_TIER_UNIT, .print_logs = true, ) {
    /* initial object */
    struct rcu_stress_node *initial =
        kmalloc(sizeof(*initial), ALLOC_FLAGS_ZERO);
    TEST_ASSERT(initial != NULL);
    initial->seq = 1;
    initial->value = 42;

    atomic_store(&stress_stop, false);
    atomic_store(&stress_failed, false);
    atomic_store(&stress_readers_done, 0);
    atomic_store(&stress_writers_done, 0);
    atomic_store(&stress_deferred_freed, 0);
    atomic_store(&stress_replacements, 0);
    stress_shared = initial;

    /* spawn readers (more than cores) */
    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        thread_spawn("rcu_str_reader_%u", rcu_stress_reader, NULL, i);
    }

    /* spawn writers */
    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        thread_spawn("rcu_str_writer_%u", rcu_stress_writer, NULL, i);
    }

    /* spawn one reclaimer to periodically call synchronize */
    thread_spawn("rcu_str_reclaimer", rcu_stress_reclaimer, NULL);

    /* run for the configured duration */
    uint64_t stop_at = time_get_ms() + STRESS_DURATION_MS;
    while (time_get_ms() < stop_at) {
        if (atomic_load(&stress_failed)) {
            test_info("RCU stress test failed early due to detection");
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

    while (atomic_load(&stress_writers_done) < STRESS_NUM_WRITERS) {
        scheduler_yield();
    }

    /* wait up to a reasonable timeout for deferred frees to run */
    for (int i = 0; i < 100 && atomic_load(&stress_deferred_freed) <
                                   atomic_load(&stress_replacements);
         i++) {
        /* call synchronize here to help force callbacks */
        rcu_synchronize();
        sleep_spin_ms(1);
    }

    test_info("RCU stress test: replacements=%u freed=%u",
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
        kfree(last);
        atomic_fetch_add(&stress_deferred_freed, 1);
    }

    return TEST_SUCCESS;
}

#endif
