#include <mem/alloc.h>
#include <misc/minheap.h>
#include <tests.h>

#define MINHEAP_TEST_TIMES 50

static struct minheap_node *nodes[MINHEAP_TEST_TIMES] = {0};

static void mhtest_do_inserts(struct minheap *mh) {
    for (int i = 0; i < MINHEAP_TEST_TIMES; i++) {
        struct minheap_node *mhn = kzalloc(sizeof(struct minheap_node));
        mhn->key = MINHEAP_TEST_TIMES - i;
        nodes[i] = mhn;
        minheap_insert(mh, mhn, mhn->key);
    }
}

REGISTER_TEST(minheap_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
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

    SET_SUCCESS;
}
