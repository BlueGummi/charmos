#include "structures/tests/test_internal.h"

#ifdef TEST_MINHEAP
TEST_GROUP_DECLARE(minheap, .intensity_desc = {
                                .curve = SCALE_PIECEWISE_LOG,
                                .unit = "nodes",
                            });

static void mhtest_do_inserts(struct minheap *mh, struct minheap_node **nodes,
                              size_t count) {
    for (size_t i = 0; i < count; i++) {
        struct minheap_node *mhn =
            kmalloc(sizeof(struct minheap_node), ALLOC_FLAGS_ZERO);
        mhn->key = count - i;
        nodes[i] = mhn;
        minheap_insert(mh, mhn, mhn->key);
    }
}

TEST_DECLARE_UNIT(minheap, basic_ops, TEST_INTENSITY(10, 50, 1024)) {
    size_t count = ctx->intensity_val ? ctx->intensity_val : 50;
    struct minheap_node **nodes =
        kmalloc(sizeof(struct minheap_node *) * count, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(nodes);

    struct minheap *mh = minheap_create();
    mhtest_do_inserts(mh, nodes, count);
    TEST_ASSERT_EQ(mh->size, count);

    for (size_t i = 0; i < count; i++) {
        struct minheap_node *mhn = nodes[i];
        minheap_remove(mh, mhn);
        kfree(mhn);
        nodes[i] = NULL;
    }

    TEST_ASSERT_EQ(mh->size, 0);
    mhtest_do_inserts(mh, nodes, count);

    TEST_ASSERT_EQ(minheap_peek(mh)->key, 1);
    struct minheap_node *popped = minheap_pop(mh);
    TEST_ASSERT_EQ(popped->key, 1);

    kfree(nodes);
    return TEST_SUCCESS;
}
#endif
