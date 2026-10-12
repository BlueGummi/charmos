#include "structures/tests/test_internal.h"

TEST_GROUP_DEFINE(minheap, .intensity_desc = {
                               .curve = SCALE_PIECEWISE_LOG,
                               .unit = "nodes",
                           });

static void mhtest_do_inserts(struct minheap *mh, struct minheap_node **nodes,
                              size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct minheap_node *mhn = vnew(mhn, ALLOC_ZERO);
        MINHEAP_NODE_SET_KEY(mhn, count - i);
        nodes[i] = mhn;
        minheap_insert(mh, mhn, MINHEAP_NODE_KEY(mhn));
    }
}

TEST_DEFINE_UNIT(minheap, insert_remove_pop_order,
                 TEST_INTENSITY(10, 50, 1024)) {
    size_t count = ctx->intensity_val ? ctx->intensity_val : 50;
    struct minheap_node **nodes = vnews(nodes, count, ALLOC_ZERO);
    TEST_ASSERT_NONNULL(nodes);

    struct minheap *mh = minheap_create();
    mhtest_do_inserts(mh, nodes, count);
    TEST_ASSERT_EQ(MINHEAP_SIZE(mh), count);

    for (size_t i = 0; i < count; i++) {
        struct minheap_node *mhn = nodes[i];
        minheap_remove(mh, mhn);
        vfree(mhn);
        nodes[i] = NULL;
    }

    TEST_ASSERT_EQ(MINHEAP_SIZE(mh), 0);
    mhtest_do_inserts(mh, nodes, count);

    TEST_ASSERT_EQ(MINHEAP_NODE_KEY(minheap_peek(mh)), 1);
    struct minheap_node *popped = minheap_pop(mh);
    TEST_ASSERT_EQ(MINHEAP_NODE_KEY(popped), 1);

    vfree(nodes);
    return TEST_SUCCESS;
}
