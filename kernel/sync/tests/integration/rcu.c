#include "sync/tests/test_internal.h"
#include <acpi/lapic.h>
#include <irq/ipi.h>
#include <irq/irq.h>
#include <mem/nofail.h>

TEST_GROUP_DEFINE(rcu, .intensity_desc = {
                           .curve = SCALE_PIECEWISE_LOG,
                           .unit = "ms",
                       });

#define NUM_RCU_READERS (global.core_count)
static size_t rcu_test_duration_ms = 50;

struct rcu_test_data {
    int value;
    struct rcu_cb rcu;
};

static atomic(struct rcu_test_data *) shared_ptr = NULL;
static atomic_bool rcu_test_failed = false;
static atomic_uint32_t rcu_reads_done = 0;

static void rcu_reader_thread(void *arg) {
    cc_unused(arg);
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

    atomic_inc(&rcu_reads_done);
}

static atomic_bool volatile rcu_deferred_freed = false;

static void rcu_free_fn(struct rcu_cb *cb) {
    vfree(container_of(cb, struct rcu_test_data, rcu));
    atomic_store(&rcu_deferred_freed, true);
}

static void rcu_writer_thread(void *arg) {
    cc_unused(arg);
    sleep_spin_ms(30);

    struct rcu_test_data *old = atomic_load_rlx(&shared_ptr);

    struct rcu_test_data *new = nofail_vnew(new, ALLOC_ZERO);
    new->value = 43;
    rcu_assign_pointer(shared_ptr, new);

    rcu_synchronize();
    rcu_defer(&old->rcu, rcu_free_fn);
}

TEST_DEFINE_INTEGRATION(rcu, mt_readers_during_replace,
                        TEST_INTENSITY(40, 50, 200)) {
    rcu_test_duration_ms = ctx->intensity_val ? ctx->intensity_val : 50;
    if (rcu_test_duration_ms < 40)
        rcu_test_duration_ms = 40;

    atomic_store(&rcu_test_failed, false);
    atomic_store(&rcu_reads_done, 0);
    atomic_store(&rcu_deferred_freed, false);

    struct rcu_test_data *initial = vnew(initial, ALLOC_ZERO);
    TEST_ASSERT_NONNULL(initial);
    initial->value = 42;
    rcu_assign_pointer(shared_ptr, initial);

    struct thread *readers[NUM_RCU_READERS];
    memset(readers, 0, sizeof(readers));
    for (uint64_t i = 0; i < NUM_RCU_READERS; i++)
        readers[i] = thread_spawn("rcu_reader_test", rcu_reader_thread,
                                  .joinable = true);

    struct thread *writer =
        thread_spawn("rcu_writer_test", rcu_writer_thread, .joinable = true);

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
    struct rcu_cb rcu;
};

static atomic(struct rcu_stress_node *) stress_shared = NULL;

static atomic_bool stress_stop = false;
static atomic_bool stress_failed = false;
static atomic_uint32_t stress_readers_done = 0;
static atomic_uint32_t stress_writers_done = 0;
static atomic_uint32_t stress_deferred_freed = 0;
static atomic_uint32_t stress_replacements = 0;
static atomic_size_t gen_freed = 0;

static void stress_free_cb(struct rcu_cb *cb) {
    atomic_store(&gen_freed, cb->gen_when_called);
    struct rcu_stress_node *n = container_of(cb, struct rcu_stress_node, rcu);
    n->value = 34;
    n->freed_gen = cb->gen_when_called;
    n->enqueued_on = cb->enqueued_waiting_on_gen;
    atomic_inc(&stress_deferred_freed);
    vfree(n);
}

static void rcu_stress_reader(void *arg) TSA_NO_ANALYSIS {
    cc_unused(arg);

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
            cc_unused(seq);
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

    atomic_inc(&stress_readers_done);
}

static void rcu_stress_writer(void *arg) {
    cc_unused(arg);
    uint64_t local_iter = 0;

    while (!atomic_load(&stress_stop)) {
        struct rcu_stress_node *new = vnew(new, ALLOC_ZERO);
        if (!new) {
            atomic_store(&stress_failed, true);
            test_info("RCU stress writer valloc failed");
            break;
        }

        new->seq = (uint64_t) atomic_fetch_add(&stress_replacements, 1) + 1;
        new->value = (local_iter & 1) ? 43 : 42;
        local_iter++;

        struct rcu_stress_node *old = atomic_xchg_acq_rel(&stress_shared, new);

        if (old)
            rcu_defer(&old->rcu, stress_free_cb);

        if ((local_iter & 0x1f) == 0) {
            rcu_synchronize();
        }

        scheduler_yield();
    }

    atomic_inc(&stress_writers_done);
}

static void rcu_stress_reclaimer(void *arg) {
    cc_unused(arg);
    while (!atomic_load(&stress_stop)) {
        rcu_synchronize();
        sleep_spin_ms(5);
    }
}

TEST_DEFINE_INTEGRATION(rcu, mt_stress, TEST_INTENSITY(200, 2000, 10000)) {
    rcu_stress_duration_ms = ctx->intensity_val ? ctx->intensity_val : 2000;
    atomic_store(&stress_stop, false);
    atomic_store(&stress_failed, false);
    atomic_store(&stress_readers_done, 0);
    atomic_store(&stress_writers_done, 0);
    atomic_store(&stress_deferred_freed, 0);
    atomic_store(&stress_replacements, 0);
    atomic_store(&gen_freed, 0);

    struct rcu_stress_node *initial = vnew(initial, ALLOC_ZERO);
    TEST_ASSERT_NONNULL(initial);
    initial->seq = 0;
    initial->value = 42;
    rcu_assign_pointer(stress_shared, initial);

    struct thread *readers[STRESS_NUM_READERS];
    struct thread *writers[STRESS_NUM_WRITERS];
    memset(readers, 0, sizeof(readers));
    memset(writers, 0, sizeof(writers));

    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        readers[i] = thread_spawn(("rcu_stread_%u", i), rcu_stress_reader,
                                  .joinable = true);
    }

    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        writers[i] = thread_spawn(("rcu_strite_%u", i), rcu_stress_writer,
                                  .joinable = true);
    }

    struct thread *reclaimer =
        thread_spawn("rcu_streclaim", rcu_stress_reclaimer, .joinable = true);

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

    struct rcu_stress_node *last = atomic_load_rlx(&stress_shared);
    if (last) {
        rcu_synchronize();
        vfree(last);
        atomic_inc(&stress_deferred_freed);
    }

    return TEST_SUCCESS;
}

#define RCU_IRQ_HOLD_MS 20
#define RCU_IRQ_WAIT_MS 1000
#define RCU_IRQ_NODE_LIVE 0x5a5a
#define RCU_IRQ_NODE_DEAD 0xdead

struct rcu_irq_node {
    atomic_int value;
    struct rcu_cb rcu;
};

static struct rcu_irq_node rcu_irq_nodes[2];
static atomic(struct rcu_irq_node *) rcu_irq_shared = NULL;
static atomic_bool rcu_irq_in_section = false;
static atomic_bool rcu_irq_done = false;
static atomic_bool rcu_irq_freed = false;
static atomic_bool rcu_irq_failed = false;
static irq_t rcu_irq_vector;
static bool rcu_irq_registered = false;

static void rcu_irq_free_cb(struct rcu_cb *cb) {
    struct rcu_irq_node *n = container_of(cb, struct rcu_irq_node, rcu);
    atomic_store_release(&n->value, RCU_IRQ_NODE_DEAD);
    atomic_store_release(&rcu_irq_freed, true);
}

static enum irq_result rcu_irq_reader(void *ctx, uint8_t vector,
                                      struct irq_context *ictx) {
    cc_unused(ctx, vector, ictx);

    rcu_read_lock();
    struct rcu_irq_node *p = rcu_dereference(rcu_irq_shared);
    atomic_store_release(&rcu_irq_in_section, true);

    /* Long enough for the writer to swap, defer, and a GP to complete if
     * this CPU is (wrongly) considered quiescent */
    time_ms_t end = time_get_ms() + RCU_IRQ_HOLD_MS;
    while (time_get_ms() < end) {
        if (atomic_load_acq(&p->value) != RCU_IRQ_NODE_LIVE) {
            atomic_store(&rcu_irq_failed, true);
            break;
        }
        cpu_pause();
    }

    rcu_read_unlock();
    atomic_store_release(&rcu_irq_done, true);
    return IRQ_HANDLED;
}

static bool rcu_irq_wait_for(atomic_bool *flag) {
    time_ms_t end = time_get_ms() + RCU_IRQ_WAIT_MS;
    while (!atomic_load_acq(flag)) {
        if (time_get_ms() >= end)
            return false;

        scheduler_yield();
    }

    return true;
}

TEST_DEFINE_INTEGRATION(rcu, irq_reader_on_idle_cpu, TEST_INTENSITY(5, 10, 100),
                        .min_cores = 4) {
    size_t rounds = ctx->intensity_val ? ctx->intensity_val : 10;

    if (!rcu_irq_registered) {
        rcu_irq_vector = irq_alloc_entry();
        irq_register("rcu_irq_reader_test", rcu_irq_vector, rcu_irq_reader,
                     NULL, IRQ_FLAG_NONE);
        irq_set_chip(rcu_irq_vector, lapic_get_chip(), NULL);
        rcu_irq_registered = true;
    }

    struct thread *self_thread = thread_get_current();
    bool was_pinned = thread_pin(self_thread);
    cpu_id_t self = smp_id(TOPC_PINNED);
    cpu_id_t target = (self + global.core_count / 2) % global.core_count;

    atomic_store(&rcu_irq_failed, false);

    size_t cur = 0;
    atomic_store(&rcu_irq_nodes[cur].value, RCU_IRQ_NODE_LIVE);
    rcu_assign_pointer(rcu_irq_shared, &rcu_irq_nodes[cur]);

    bool timed_out = false;
    for (size_t r = 0; r < rounds && !timed_out; r++) {
        size_t next = cur ^ 1;
        atomic_store(&rcu_irq_nodes[next].value, RCU_IRQ_NODE_LIVE);

        atomic_store(&rcu_irq_in_section, false);
        atomic_store(&rcu_irq_done, false);
        atomic_store(&rcu_irq_freed, false);

        /* Let the target settle back into its idle loop */
        sleep_spin_ms(1);
        ipi_send(target, rcu_irq_vector);

        if (!rcu_irq_wait_for(&rcu_irq_in_section)) {
            timed_out = true;
            break;
        }

        rcu_assign_pointer(rcu_irq_shared, &rcu_irq_nodes[next]);
        rcu_defer(&rcu_irq_nodes[cur].rcu, rcu_irq_free_cb);

        if (!rcu_irq_wait_for(&rcu_irq_done)) {
            timed_out = true;
            break;
        }

        rcu_synchronize();
        if (!rcu_irq_wait_for(&rcu_irq_freed)) {
            timed_out = true;
            break;
        }

        cur = next;
        if (atomic_load(&rcu_irq_failed))
            break;
    }

    if (!was_pinned)
        thread_unpin(self_thread);

    TEST_ASSERT(!timed_out);
    TEST_ASSERT(!atomic_load(&rcu_irq_failed));

    return TEST_SUCCESS;
}
