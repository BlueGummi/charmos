#include "../test_internal.h"

#ifdef DEBUG_LOCK_CHK

#include <sync/lock_chk_internal.h>
#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/spinlock.h>

LOCK_CHK_CLASS_DECLARE_LOCAL(graph_test_class_a);
LOCK_CHK_CLASS_DECLARE_LOCAL(graph_test_class_b);
LOCK_CHK_CLASS_DECLARE_LOCAL(graph_test_class_c);

TEST_DECLARE_UNIT(lock_chk_graph_node_resolution,
                  .group = TEST_GROUP(qspinlock)) {
    static struct lock_chk_graph graph;
    lock_chk_graph_init(&graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));

    struct lock_chk_node *node_a0 = NULL;
    struct lock_chk_node *node_a0_again = NULL;
    struct lock_chk_node *node_a1 = NULL;
    struct lock_chk_node *node_b0 = NULL;

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL,
                                            &node_a0) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(node_a0 != NULL);

    TEST_ASSERT(
        lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a0_again) ==
        LOCK_CHK_RESULT_OK);
    TEST_ASSERT(node_a0 == node_a0_again);

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 1, NULL,
                                            &node_a1) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(node_a1 != NULL && node_a1 != node_a0);
    TEST_ASSERT(node_a1->subclass == 1);

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_b, 0, NULL,
                                            &node_b0) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(node_b0 != NULL && node_b0 != node_a0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_graph_cycle_detection,
                  .group = TEST_GROUP(qspinlock)) {
    static struct lock_chk_graph graph;
    lock_chk_graph_init(&graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_map map_c =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_c));

    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;
    struct lock_chk_node *node_c = NULL;

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a) ==
                LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_b, 0, NULL, &node_b) ==
                LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_c, 0, NULL, &node_c) ==
                LOCK_CHK_RESULT_OK);

    /* A -> B */
    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_a, LOCK_CHK_MODE_EXCLUSIVE, node_b,
                    LOCK_CHK_MODE_EXCLUSIVE, NULL, NULL) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(graph.edge_count == 1);

    /* Duplicate A -> B should be dedup without adding edge */
    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_a, LOCK_CHK_MODE_EXCLUSIVE, node_b,
                    LOCK_CHK_MODE_EXCLUSIVE, NULL, NULL) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(graph.edge_count == 1);

    /* B -> C */
    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_b, LOCK_CHK_MODE_EXCLUSIVE, node_c,
                    LOCK_CHK_MODE_EXCLUSIVE, NULL, NULL) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(graph.edge_count == 2);

    /* C -> A completes cycle A -> B -> C -> A reports CYCLE */
    struct lock_chk_failure fail = {0};
    TEST_ASSERT(lock_chk_graph_add_dependency(&graph, node_c,
                                              LOCK_CHK_MODE_EXCLUSIVE, node_a,
                                              LOCK_CHK_MODE_EXCLUSIVE, NULL,
                                              &fail) == LOCK_CHK_RESULT_CYCLE);
    TEST_ASSERT(fail.cycle_len == 3);
    TEST_ASSERT(fail.signature != 0);

    /* Direct B -> A completes 2-node cycle: must report CYCLE */
    TEST_ASSERT(lock_chk_graph_add_dependency(&graph, node_b,
                                              LOCK_CHK_MODE_EXCLUSIVE, node_a,
                                              LOCK_CHK_MODE_EXCLUSIVE, NULL,
                                              NULL) == LOCK_CHK_RESULT_CYCLE);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_irq_safety_conflict,
                  .group = TEST_GROUP(qspinlock)) {
    static struct lock_chk_graph graph;
    lock_chk_graph_init(&graph);

    struct lock_chk_map map_disp =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_high =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));

    struct lock_chk_acquire_request req_disp = {
        .type = LOCK_CHK_TYPE_SPIN,
        .prev_irql = IRQL_PASSIVE_LEVEL,
        .irqs_enabled = true,
        .irq_safe = false,
        .raw_operation = false,
        .in_irq = false,
        .in_nmi = false,
    };
    struct lock_chk_acquire_request req_high = {
        .type = LOCK_CHK_TYPE_SPIN,
        .prev_irql = IRQL_HIGH_LEVEL,
        .irqs_enabled = false,
        .irq_safe = true,
        .raw_operation = false,
        .in_irq = false,
        .in_nmi = false,
    };

    struct lock_chk_node *node = NULL;

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_disp, 0, &req_disp,
                                            &node) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(
        lock_chk_graph_resolve_node(&graph, &map_disp, 0, &req_high, &node) ==
        LOCK_CHK_RESULT_BAD_CONTEXT);

    node = NULL;
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_high, 0, &req_high,
                                            &node) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(
        lock_chk_graph_resolve_node(&graph, &map_high, 0, &req_disp, &node) ==
        LOCK_CHK_RESULT_BAD_CONTEXT);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_graph_acquire_batch_deduplicates,
                  .group = TEST_GROUP(qspinlock)) {
    static struct lock_chk_graph graph;
    lock_chk_graph_init(&graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a) ==
                LOCK_CHK_RESULT_OK);

    struct lock_chk_thread_data held = {
        .held =
            {
                {.node = node_a,
                 .flags = LOCK_CHKD_ORDER,
                 .mode = LOCK_CHK_MODE_EXCLUSIVE},
                {.node = node_a,
                 .flags = LOCK_CHKD_ORDER,
                 .mode = LOCK_CHK_MODE_EXCLUSIVE},
            },
        .depth = 2,
    };
    struct lock_chk_acquire_request request = {
        .map = &map_b,
        .flags = LOCK_CHKD_ORDER,
        .type = LOCK_CHK_TYPE_MUTEX,
        .mode = LOCK_CHK_MODE_EXCLUSIVE,
        .wait_kind = LOCK_CHK_WAIT_BLOCKING,
    };
    struct lock_chk_failure failure = {0};

    TEST_ASSERT(lock_chk_graph_prepare_acquire(&graph, &map_b, 0, &request,
                                               &held, &node_b,
                                               &failure) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(node_b != NULL);
    TEST_ASSERT(graph.edge_count == 1);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_graph_acquire_batch_rolls_back,
                  .group = TEST_GROUP(qspinlock)) {
    static struct lock_chk_graph graph;
    lock_chk_graph_init(&graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));
    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a) ==
                LOCK_CHK_RESULT_OK);

    struct lock_chk_thread_data held = {
        .held = {{.node = node_a,
                  .flags = LOCK_CHKD_ORDER,
                  .mode = LOCK_CHK_MODE_EXCLUSIVE}},
        .depth = 1,
    };
    struct lock_chk_acquire_request request = {
        .map = &map_b,
        .flags = LOCK_CHKD_ORDER,
        .type = LOCK_CHK_TYPE_MUTEX,
        .mode = LOCK_CHK_MODE_EXCLUSIVE,
        .wait_kind = LOCK_CHK_WAIT_BLOCKING,
    };
    struct lock_chk_failure failure = {0};
    uint16_t nodes_before = graph.node_count;
    graph.edge_count = LOCK_CHK_MAX_EDGES;

    TEST_ASSERT(lock_chk_graph_prepare_acquire(&graph, &map_b, 0, &request,
                                               &held, &node_b, &failure) ==
                LOCK_CHK_RESULT_EDGE_CAPACITY);
    TEST_ASSERT(node_b == NULL);
    TEST_ASSERT(graph.node_count == nodes_before);
    TEST_ASSERT(atomic_load_explicit(&map_b.base_node, memory_order_relaxed) ==
                NULL);
    TEST_ASSERT(failure.kind == LOCK_CHK_FAIL_CAPACITY);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_spin_qspin_deep_lifecycle,
                  .group = TEST_GROUP(qspinlock)) {
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

    irql = spin_lock_irq_disable(&spin_irq);
    spin_unlock(&spin_irq, irql);

    TEST_ASSERT(spin_trylock_irq_disable(&spin_irq, &irql));
    spin_unlock(&spin_irq, irql);

    spin_lock_raw(&spin_raw);
    spin_unlock_raw(&spin_raw);

    TEST_ASSERT(spin_trylock_raw(&spin_raw));
    spin_unlock_raw(&spin_raw);

    irql = qspin_lock(&qspin_disp);
    qspin_unlock(&qspin_disp, irql);

    TEST_ASSERT(qspin_trylock(&qspin_disp, &irql));
    qspin_unlock(&qspin_disp, irql);

    irql = qspin_lock_irq_disable(&qspin_irq);
    qspin_unlock(&qspin_irq, irql);

    TEST_ASSERT(qspin_trylock_irq_disable(&qspin_irq, &irql));
    qspin_unlock(&qspin_irq, irql);

    qspin_lock_raw(&qspin_raw);
    qspin_unlock_raw(&qspin_raw);

    TEST_ASSERT(qspin_trylock_raw(&qspin_raw));
    qspin_unlock_raw(&qspin_raw);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_mutex_out_of_order_release,
                  .group = TEST_GROUP(qspinlock)) {
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

TEST_DECLARE_UNIT(lock_chk_mutex_simple_lifecycle,
                  .group = TEST_GROUP(qspinlock)) {
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

TEST_DECLARE_UNIT(lock_chk_rw_reader_ring_and_conflict,
                  .group = TEST_GROUP(qspinlock)) {
    static struct lock_chk_graph graph;
    lock_chk_graph_init(&graph);

    struct lock_chk_map map_a =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_a));
    struct lock_chk_map map_b =
        LOCK_CHK_MAP_VALUE_INIT(LOCK_CHK_CLASS(graph_test_class_b));

    struct lock_chk_node *node_a = NULL;
    struct lock_chk_node *node_b = NULL;

    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a) ==
                LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_b, 0, NULL, &node_b) ==
                LOCK_CHK_RESULT_OK);

    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_a, LOCK_CHK_MODE_SHARED, node_b,
                    LOCK_CHK_MODE_SHARED, NULL, NULL) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_b, LOCK_CHK_MODE_SHARED, node_a,
                    LOCK_CHK_MODE_SHARED, NULL, NULL) == LOCK_CHK_RESULT_OK);

    lock_chk_graph_init(&graph);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a) ==
                LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_b, 0, NULL, &node_b) ==
                LOCK_CHK_RESULT_OK);

    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_a, LOCK_CHK_MODE_SHARED, node_b,
                    LOCK_CHK_MODE_EXCLUSIVE, NULL, NULL) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_add_dependency(&graph, node_b,
                                              LOCK_CHK_MODE_SHARED, node_a,
                                              LOCK_CHK_MODE_EXCLUSIVE, NULL,
                                              NULL) == LOCK_CHK_RESULT_CYCLE);

    lock_chk_graph_init(&graph);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_a, 0, NULL, &node_a) ==
                LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_resolve_node(&graph, &map_b, 0, NULL, &node_b) ==
                LOCK_CHK_RESULT_OK);

    TEST_ASSERT(lock_chk_graph_add_dependency(
                    &graph, node_a, LOCK_CHK_MODE_EXCLUSIVE, node_b,
                    LOCK_CHK_MODE_EXCLUSIVE, NULL, NULL) == LOCK_CHK_RESULT_OK);
    TEST_ASSERT(lock_chk_graph_add_dependency(&graph, node_b,
                                              LOCK_CHK_MODE_EXCLUSIVE, node_a,
                                              LOCK_CHK_MODE_EXCLUSIVE, NULL,
                                              NULL) == LOCK_CHK_RESULT_CYCLE);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(lock_chk_rwlock_lifecycle, .group = TEST_GROUP(qspinlock)) {
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

TEST_DECLARE_UNIT(lock_chk_subclasses_all_primitives,
                  .group = TEST_GROUP(qspinlock)) {
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
