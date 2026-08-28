#include "../test_internal.h"

#ifdef TEST_RCU
TEST_GROUP_DECLARE(rcu, .intensity_desc = {
                            .curve = SCALE_PIECEWISE_LOG,
                            .unit = "ms",
                        });

#define NUM_RCU_READERS (global.core_count)
static size_t rcu_test_duration_ms = 50;

struct rcu_test_data {
    int value;
};

static _Atomic(struct rcu_test_data *) shared_ptr = NULL;
static atomic_bool rcu_test_failed = false;
static _Atomic uint32_t rcu_reads_done = 0;

static void rcu_reader_thread(void *) {
    uint64_t end = time_get_ms() + rcu_test_duration_ms;

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

TEST_DECLARE_INTEGRATION(rcu, rcu_test, TEST_INTENSITY(40, 50, 200)) {
    rcu_test_duration_ms = ctx->intensity_val ? ctx->intensity_val : 50;
    if (rcu_test_duration_ms < 40)
        rcu_test_duration_ms = 40;

    atomic_store(&rcu_test_failed, false);
    atomic_store(&rcu_reads_done, 0);
    atomic_store(&rcu_deferred_freed, false);

    struct rcu_test_data *initial = kmalloc(sizeof(*initial), ALLOC_FLAGS_ZERO);
    initial->value = 42;
    shared_ptr = initial;

    struct thread *readers[NUM_RCU_READERS];
    for (uint64_t i = 0; i < NUM_RCU_READERS; i++)
        readers[i] =
            thread_spawn_joinable("rcu_reader_test", rcu_reader_thread, NULL);

    struct thread *writer =
        thread_spawn_joinable("rcu_writer_test", rcu_writer_thread, NULL);

    for (uint64_t i = 0; i < NUM_RCU_READERS; i++) {
        if (readers[i])
            thread_join(readers[i]);
    }

    if (writer)
        thread_join(writer);

    TEST_ASSERT_EQ(atomic_load(&rcu_reads_done), NUM_RCU_READERS);

    for (int i = 0; i < 100 && !atomic_load(&rcu_deferred_freed); i++)
        sleep_spin_ms(1);

    TEST_ASSERT(!atomic_load(&rcu_test_failed));

    return TEST_SUCCESS;
}

#define STRESS_NUM_READERS (global.core_count * 8)
#define STRESS_NUM_WRITERS (global.core_count)
static size_t rcu_stress_duration_ms = 2000;
#define STRESS_PRINT_MS 1000

struct rcu_stress_node {
    uint64_t seq; /* monotonic sequence number (for debugging) */
    int value;
    size_t freed_gen, enqueued_on;
};

static _Atomic(struct rcu_stress_node *) stress_shared = NULL;

static atomic_bool stress_stop = false;
static atomic_bool stress_failed = false;
static _Atomic uint32_t stress_readers_done = 0;
static _Atomic uint32_t stress_writers_done = 0;
static _Atomic uint32_t stress_deferred_freed = 0;
static _Atomic uint32_t stress_replacements = 0;
static atomic_size_t gen_freed = 0;

static void stress_free_cb(struct rcu_cb *cb, void *ptr) {
    atomic_store(&gen_freed, cb->gen_when_called);
    struct rcu_stress_node *n = ptr;
    n->value = 34;
    n->freed_gen = cb->gen_when_called;
    n->enqueued_on = cb->enqueued_waiting_on_gen;
    atomic_fetch_add(&stress_deferred_freed, 1);
    kfree(cb);
    kfree(n);
}

static void rcu_stress_reader(void *arg) {
    (void) arg;

    time_ms_t last_print = time_get_ms();
    size_t iter = 0;
    while (!atomic_load(&stress_stop)) {
        rcu_read_lock();

        struct rcu_stress_node *p = rcu_dereference(stress_shared);
        if (p) {
            int v = p->value;
            if (v != 42 && v != 43) {
                atomic_store(&stress_failed, true);
                test_err("RCU stress reader saw invalid value");
                break;
            }
            volatile uint64_t seq = p->seq;
            (void) seq;
        }

        rcu_read_unlock();

        if (time_get_ms() - last_print > STRESS_PRINT_MS) {
            last_print = time_get_ms();
            test_info("\'%-17s\' iter %7zu w/ %7zu rplace and %7zu free",
                      thread_get_current()->name, iter,
                      (size_t) atomic_load(&stress_replacements),
                      (size_t) atomic_load(&stress_deferred_freed));
        }

        scheduler_yield();
        iter++;
    }

    atomic_fetch_add(&stress_readers_done, 1);
}

static void rcu_stress_writer(void *arg) {
    (void) arg;
    uint64_t local_iter = 0;

    while (!atomic_load(&stress_stop)) {
        struct rcu_stress_node *new = kmalloc(sizeof(*new), ALLOC_FLAGS_ZERO);
        if (!new) {
            atomic_store(&stress_failed, true);
            test_info("RCU stress writer kmalloc failed");
            break;
        }

        new->seq = (uint64_t) atomic_fetch_add(&stress_replacements, 1) + 1;
        new->value = (local_iter & 1) ? 43 : 42;
        local_iter++;

        struct rcu_stress_node *old =
            atomic_exchange_explicit(&stress_shared, new, memory_order_acq_rel);

        if (old)
            rcu_defer(kmalloc(sizeof(struct rcu_cb), ALLOC_FLAGS_ZERO),
                      stress_free_cb, old);

        if ((local_iter & 0x1f) == 0) {
            rcu_synchronize();
        }

        scheduler_yield();
    }

    atomic_fetch_add(&stress_writers_done, 1);
}

static void rcu_stress_reclaimer(void *arg) {
    (void) arg;
    while (!atomic_load(&stress_stop)) {
        rcu_synchronize();
        sleep_spin_ms(5);
    }
}

TEST_DECLARE_INTEGRATION(rcu, rcu_stress_test,
                         TEST_INTENSITY(200, 2000, 10000)) {
    rcu_stress_duration_ms = ctx->intensity_val ? ctx->intensity_val : 2000;
    atomic_store(&stress_stop, false);
    atomic_store(&stress_failed, false);
    atomic_store(&stress_readers_done, 0);
    atomic_store(&stress_writers_done, 0);
    atomic_store(&stress_deferred_freed, 0);
    atomic_store(&stress_replacements, 0);
    atomic_store(&gen_freed, 0);

    struct rcu_stress_node *initial =
        kmalloc(sizeof(*initial), ALLOC_FLAGS_ZERO);
    initial->seq = 0;
    initial->value = 42;
    stress_shared = initial;

    struct thread *readers[STRESS_NUM_READERS];
    struct thread *writers[STRESS_NUM_WRITERS];

    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        readers[i] =
            thread_spawn_joinable("rcu_stread_%u", rcu_stress_reader, NULL, i);
    }

    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        writers[i] =
            thread_spawn_joinable("rcu_strite_%u", rcu_stress_writer, NULL, i);
    }

    struct thread *reclaimer =
        thread_spawn_joinable("rcu_streclaim", rcu_stress_reclaimer, NULL);

    uint64_t stop_at = time_get_ms() + rcu_stress_duration_ms;
    while (time_get_ms() < stop_at) {
        if (atomic_load(&stress_failed)) {
            test_info("RCU stress test failed early due to detection");
            break;
        }
        scheduler_yield();
    }

    atomic_store(&stress_stop, true);

    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        if (readers[i])
            thread_join(readers[i]);
    }

    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        if (writers[i])
            thread_join(writers[i]);
    }

    if (reclaimer)
        thread_join(reclaimer);

    for (int i = 0; i < 100 && atomic_load(&stress_deferred_freed) <
                                   atomic_load(&stress_replacements);
         i++) {
        rcu_synchronize();
        sleep_spin_ms(1);
    }

    test_info("RCU stress test: replacements=%u freed=%u",
              (unsigned) atomic_load(&stress_replacements),
              (unsigned) atomic_load(&stress_deferred_freed));
    test_info(
        " [RCU STATS] Completed %u replacements, %u deferred frees across "
        "64 readers & 8 writers\n",
        (unsigned) atomic_load(&stress_replacements),
        (unsigned) atomic_load(&stress_deferred_freed));

    TEST_ASSERT(!atomic_load(&stress_failed));
    TEST_ASSERT_GT(atomic_load(&stress_deferred_freed), 0);
    TEST_ASSERT_EQ(atomic_load(&stress_deferred_freed),
                   atomic_load(&stress_replacements));

    struct rcu_stress_node *last = stress_shared;
    if (last) {
        rcu_synchronize();
        kfree(last);
        atomic_fetch_add(&stress_deferred_freed, 1);
    }

    return TEST_SUCCESS;
}
#endif
