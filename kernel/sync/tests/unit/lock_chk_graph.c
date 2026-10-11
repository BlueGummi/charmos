#include "sync/lock_chk/internal.h"
#include "sync/tests/test_internal.h"

#ifdef DEBUG_LOCK_CHK

#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>

LOCK_CHK_CLASS_DEFINE_LOCAL(graph_test_class_a);
LOCK_CHK_CLASS_DEFINE_LOCAL(graph_test_class_b);
LOCK_CHK_CLASS_DEFINE_LOCAL(graph_test_class_c);

static struct lock_chk_graph lock_chk_test_graph;

#define TEST_CTX(g, ...) (&(struct lock_chk_ctx){.graph = (g), __VA_ARGS__})

TEST_DEFINE_UNIT(lock_chk, graph_node_resolution) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));

    struct lock_chk_node *node_a0 = NULL;
    struct lock_chk_node *node_a0_again = NULL;
    struct lock_chk_node *node_a1 = NULL;
    struct lock_chk_node *node_b0 = NULL;

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a0),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_NONNULL(node_a0);

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a0_again),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_PTR_EQ(node_a0, node_a0_again);

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 1, &node_a1),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_NONNULL(node_a1);
    TEST_ASSERT_PTR_NE(node_a1, node_a0);
    TEST_ASSERT_EQ(node_a1->subclass, 1);

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_b, 0, &node_b0),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_NONNULL(node_b0);
    TEST_ASSERT_PTR_NE(node_b0, node_a0);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, graph_cycle_detection) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_map map_c =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_c));

    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;
    struct lock_chk_node *node_c = NULL;

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_b, 0, &node_b),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_c, 0, &node_c),
        LOCK_CHK_RESULT_OK);

    /* A -> B */
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(graph->edge_count, 1);

    /* Duplicate A -> B should be dedup without adding edge */
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(graph->edge_count, 1);

    /* B -> C */
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_c, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(graph->edge_count, 2);

    /* C -> A completes cycle A -> B -> C -> A reports CYCLE */
    struct lock_chk_fault fail = {0};
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph, .fault = &fail),
                       lock_chk_dep_make(node_c, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_CYCLE);
    TEST_ASSERT_NONNULL(fail.report);
    TEST_ASSERT_EQ(fail.report->cycle_len, 3);
    TEST_ASSERT_NE(fail.report->signature, 0);
    lock_chk_report_release();

    /* Direct B -> A completes 2-node cycle: must report CYCLE */
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_CYCLE);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, irq_safety_conflict) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_disp =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_high =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));

    struct lock_chk_acq_req req_disp = {
        .lock.type = LOCK_CHK_TYPE_SPIN,
        .prev_irql = IRQL_PASSIVE_LEVEL,
        .irqs_enabled = true,
        .op_flags = LOCK_OP_IRQ_DISPATCH,
        .in_irq = false,
    };
    struct lock_chk_acq_req req_high = {
        .lock.type = LOCK_CHK_TYPE_SPIN,
        .prev_irql = IRQL_HIGH_LEVEL,
        .irqs_enabled = false,
        .op_flags = LOCK_OP_IRQ_HIGH,
        .in_irq = false,
    };

    struct lock_chk_node *node = NULL;

    TEST_ASSERT_EQ(lock_chk_graph_resolve_node(
                       TEST_CTX(graph, .req = &req_disp), &map_disp, 0, &node),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(lock_chk_graph_resolve_node(
                       TEST_CTX(graph, .req = &req_high), &map_disp, 0, &node),
                   LOCK_CHK_RESULT_BAD_CONTEXT);

    node = NULL;
    TEST_ASSERT_EQ(lock_chk_graph_resolve_node(
                       TEST_CTX(graph, .req = &req_high), &map_high, 0, &node),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(lock_chk_graph_resolve_node(
                       TEST_CTX(graph, .req = &req_disp), &map_high, 0, &node),
                   LOCK_CHK_RESULT_BAD_CONTEXT);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, graph_acquire_dedup) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);

    struct lock_chk_thread_data held = {
        .held =
            {
                {.node = node_a,
                 .lock.flags = LOCK_CHKD_ORDER,
                 .mode = LOCK_CHK_MODE_EXCLUSIVE},
                {.node = node_a,
                 .lock.flags = LOCK_CHKD_ORDER,
                 .mode = LOCK_CHK_MODE_EXCLUSIVE},
            },
        .depth = 2,
    };
    struct lock_chk_acq_req request = {
        .map = &map_b,
        .lock.flags = LOCK_CHKD_ORDER,
        .lock.type = LOCK_CHK_TYPE_MUTEX,
        .mode = LOCK_CHK_MODE_EXCLUSIVE,
        .op_flags = LOCK_OP_KIND_BLOCKING,
    };
    struct lock_chk_fault failure = {0};

    TEST_ASSERT_EQ(lock_chk_graph_prepare_acq(TEST_CTX(graph, .req = &request,
                                                       .thread_data = &held,
                                                       .fault = &failure),
                                              &map_b, 0, &node_b),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_NONNULL(node_b);
    TEST_ASSERT_EQ(graph->edge_count, 1);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, graph_acquire_cycle_fault_populated) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_b, 0, &node_b),
        LOCK_CHK_RESULT_OK);

    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_OK);

    struct lock_chk_thread_data held = {
        .held = {{.node = node_a,
                  .lock.flags = LOCK_CHKD_ORDER,
                  .mode = LOCK_CHK_MODE_EXCLUSIVE}},
        .depth = 1,
    };
    struct lock_chk_acq_req request = {
        .map = &map_b,
        .lock.flags = LOCK_CHKD_ORDER,
        .lock.type = LOCK_CHK_TYPE_MUTEX,
        .mode = LOCK_CHK_MODE_EXCLUSIVE,
        .op_flags = LOCK_OP_KIND_BLOCKING,
        .site = LOCK_CHK_SITE_HERE(),
    };
    struct lock_chk_fault fault = {0};
    struct lock_chk_node *out = NULL;

    TEST_ASSERT_EQ(lock_chk_graph_prepare_acq(
                       TEST_CTX(graph, .req = &request, .thread_data = &held,
                                .fault = &fault, .site = request.site),
                       &map_b, 0, &out),
                   LOCK_CHK_RESULT_CYCLE);

    TEST_ASSERT_EQ(fault.kind, LOCK_CHK_FAIL_CYCLE);
    TEST_ASSERT_NONNULL(fault.report);
    TEST_ASSERT_NE(fault.report->cycle_len, 0);
    TEST_ASSERT_NE(fault.report->signature, 0);
    TEST_ASSERT_NE(fault.report->msg[0], '\0');
    TEST_ASSERT_NONNULL(fault.class);
    lock_chk_report_release();
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, graph_acquire_rollback) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);

    struct lock_chk_thread_data held = {
        .held = {{.node = node_a,
                  .lock.flags = LOCK_CHKD_ORDER,
                  .mode = LOCK_CHK_MODE_EXCLUSIVE}},
        .depth = 1,
    };
    struct lock_chk_acq_req request = {
        .map = &map_b,
        .lock.flags = LOCK_CHKD_ORDER,
        .lock.type = LOCK_CHK_TYPE_MUTEX,
        .mode = LOCK_CHK_MODE_EXCLUSIVE,
        .op_flags = LOCK_OP_KIND_BLOCKING,
    };
    struct lock_chk_fault failure = {0};
    uint16_t nodes_before = graph->node_count;
    graph->edge_count = LOCK_CHK_MAX_EDGES;

    TEST_ASSERT_EQ(lock_chk_graph_prepare_acq(TEST_CTX(graph, .req = &request,
                                                       .thread_data = &held,
                                                       .fault = &failure),
                                              &map_b, 0, &node_b),
                   LOCK_CHK_RESULT_EDGE_CAPACITY);
    TEST_ASSERT_NULL(node_b);
    TEST_ASSERT_EQ(graph->node_count, nodes_before);
    TEST_ASSERT_NULL(atomic_load_rlx(&map_b.base_node));
    TEST_ASSERT_EQ(failure.kind, LOCK_CHK_FAIL_CAPACITY);
    lock_chk_report_release();
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, spin_qspin_lifecycle) {
    struct spinlock spin_disp;
    struct spinlock spin_irq;
    struct spinlock spin_raw;
    struct qspinlock qspin_disp;
    struct qspinlock qspin_irq;
    struct qspinlock qspin_raw;
    enum irql irql;

    spinlock_init(&spin_disp);
    spinlock_init(&spin_irq);
    spinlock_init(&spin_raw);
    qspinlock_init(&qspin_disp);
    qspinlock_init(&qspin_irq);
    qspinlock_init(&qspin_raw);

    irql = spin_lock(&spin_disp);
    spin_unlock(&spin_disp, irql);

    TEST_ASSERT(spin_trylock(&spin_disp, &irql));
    spin_unlock(&spin_disp, irql);

    irql = spin_lock_high(&spin_irq);
    spin_unlock(&spin_irq, irql);

    TEST_ASSERT(spin_trylock_high(&spin_irq, &irql));
    spin_unlock(&spin_irq, irql);

    spin_lock_raw(&spin_raw);
    spin_unlock_raw(&spin_raw);

    TEST_ASSERT(spin_trylock_raw(&spin_raw));
    spin_unlock_raw(&spin_raw);

    irql = qspin_lock(&qspin_disp);
    qspin_unlock(&qspin_disp, irql);

    TEST_ASSERT(qspin_trylock(&qspin_disp, &irql));
    qspin_unlock(&qspin_disp, irql);

    irql = qspin_lock_high(&qspin_irq);
    qspin_unlock(&qspin_irq, irql);

    TEST_ASSERT(qspin_trylock_high(&qspin_irq, &irql));
    qspin_unlock(&qspin_irq, irql);

    qspin_lock_raw(&qspin_raw);
    qspin_unlock_raw(&qspin_raw);

    TEST_ASSERT(qspin_trylock_raw(&qspin_raw));
    qspin_unlock_raw(&qspin_raw);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, mutex_out_of_order_release) {
    struct mutex m1;
    struct mutex m2;

    mutex_init(&m1);
    mutex_init(&m2);

    mutex_lock(&m1);
    mutex_lock(&m2);

    mutex_unlock(&m1);
    mutex_unlock(&m2);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, mutex_simple_lifecycle) {
    struct mutex_simple s1;
    struct mutex_simple s2;

    mutex_simple_init(&s1);
    mutex_simple_init(&s2);

    mutex_simple_lock(&s1);
    mutex_simple_unlock(&s1);

    mutex_simple_lock_subclass(&s1, 1);
    mutex_simple_unlock(&s1);

    mutex_simple_lock(&s1);
    mutex_simple_lock(&s2);

    mutex_simple_unlock(&s1);
    mutex_simple_unlock(&s2);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, rw_reader_ring_and_conflict) {
    struct lock_chk_graph *graph = &lock_chk_test_graph;
    lock_chk_graph_init(graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));

    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_b, 0, &node_b),
        LOCK_CHK_RESULT_OK);

    TEST_ASSERT_EQ(
        lock_chk_graph_add_dep(TEST_CTX(graph),
                               lock_chk_dep_make(node_a, LOCK_CHK_MODE_SHARED),
                               lock_chk_dep_make(node_b, LOCK_CHK_MODE_SHARED)),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_add_dep(TEST_CTX(graph),
                               lock_chk_dep_make(node_b, LOCK_CHK_MODE_SHARED),
                               lock_chk_dep_make(node_a, LOCK_CHK_MODE_SHARED)),
        LOCK_CHK_RESULT_OK);

    lock_chk_graph_init(graph);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_b, 0, &node_b),
        LOCK_CHK_RESULT_OK);

    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_SHARED),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_SHARED),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_CYCLE);

    lock_chk_graph_init(graph);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_a, 0, &node_a),
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(
        lock_chk_graph_resolve_node(TEST_CTX(graph), &map_b, 0, &node_b),
        LOCK_CHK_RESULT_OK);

    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_OK);
    TEST_ASSERT_EQ(lock_chk_graph_add_dep(
                       TEST_CTX(graph),
                       lock_chk_dep_make(node_b, LOCK_CHK_MODE_EXCLUSIVE),
                       lock_chk_dep_make(node_a, LOCK_CHK_MODE_EXCLUSIVE)),
                   LOCK_CHK_RESULT_CYCLE);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, rwlock_lifecycle) {
    struct rwlock rw;
    rwlock_init(&rw, THREAD_PRIO_CLASS_TIMESHARE);

    /* Read acquire and release */
    rw_read_lock(&rw);
    rw_unlock(&rw);

    /* Write acquire and release */
    rw_write_lock(&rw);
    rw_unlock(&rw);

    /* Subclass read and write */
    rw_read_lock_subclass(&rw, 1);
    rw_unlock(&rw);

    rw_write_lock_subclass(&rw, 2);
    rw_unlock(&rw);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(lock_chk, subclasses_all_primitives) {
    struct spinlock spin;
    struct qspinlock qspin;
    struct mutex mtx;
    struct mutex_simple mtx_s;
    struct rwlock rw;

    spinlock_init(&spin);
    qspinlock_init(&qspin);
    mutex_init(&mtx);
    mutex_simple_init(&mtx_s);
    rwlock_init(&rw, THREAD_PRIO_CLASS_TIMESHARE);

    for (unsigned int sc = 0; sc < 8; sc++) {
        enum irql irql;

        irql = spin_lock_subclass(&spin, sc);
        spin_unlock(&spin, irql);

        irql = qspin_lock_subclass(&qspin, sc);
        qspin_unlock(&qspin, irql);

        mutex_lock_subclass(&mtx, sc);
        mutex_unlock(&mtx);

        mutex_simple_lock_subclass(&mtx_s, sc);
        mutex_simple_unlock(&mtx_s);

        rw_read_lock_subclass(&rw, sc);
        rw_unlock(&rw);

        rw_write_lock_subclass(&rw, sc);
        rw_unlock(&rw);
    }

    return TEST_SUCCESS;
}

#endif /* DEBUG_LOCK_CHK */
