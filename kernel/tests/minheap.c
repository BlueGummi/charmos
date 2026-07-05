#ifdef TEST_MINHEAP

#include <mem/alloc.h>
#include <structures/minheap.h>
#include <test.h>

#define MINHEAP_TEST_TIMES 50

static struct minheap_node *nodes[MINHEAP_TEST_TIMES] = {0};

static void mhtest_do_inserts(struct minheap *mh) {
    for (int i = 0; i < MINHEAP_TEST_TIMES; i++) {
        struct minheap_node *mhn =
            kmalloc(sizeof(struct minheap_node), ALLOC_FLAGS_ZERO);
        mhn->key = MINHEAP_TEST_TIMES - i;
        nodes[i] = mhn;
        minheap_insert(mh, mhn, mhn->key);
    }
}

TEST_DECLARE(minheap_test, .tier = TEST_TIER_UNIT) {
    struct minheap *mh = minheap_create();
    mhtest_do_inserts(mh);
    TEST_ASSERT(mh->size == MINHEAP_TEST_TIMES);

    for (int i = 0; i < MINHEAP_TEST_TIMES; i++) {
        struct minheap_node *mhn = nodes[i];
        minheap_remove(mh, mhn);
        kfree(mhn);
        nodes[i] = NULL;
    }

    TEST_ASSERT(mh->size == 0);
    mhtest_do_inserts(mh);

    TEST_ASSERT(minheap_peek(mh)->key == 1);
    TEST_ASSERT(minheap_pop(mh)->key == 1);

    return TEST_SUCCESS;
}

#endif
