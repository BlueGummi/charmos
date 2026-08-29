#include "structures/tests/test_internal.h"

#ifdef TEST_TREAP
TEST_GROUP_DECLARE(treap);

struct test_treap_node {
    int key;
    struct treap_node node;
};

static int test_treap_cmp(const struct treap_node *a,
                          const struct treap_node *b) {
    int ka = treap_entry(a, struct test_treap_node, node)->key;
    int kb = treap_entry(b, struct test_treap_node, node)->key;
    return (ka > kb) - (ka < kb);
}

static int test_treap_cmp_key(const struct treap_node *a, const void *key) {
    int ka = treap_entry(a, struct test_treap_node, node)->key;
    int kk = *(const int *) key;
    return (ka > kk) - (ka < kk);
}

static const struct treap_node_ops test_treap_ops = {
    .cmp = test_treap_cmp,
    .cmp_key = test_treap_cmp_key,
};

static bool verify_treap_invariants(struct treap_node *n, int *min, int *max) {
    if (!n) {
        return true;
    }

    int k = treap_entry(n, struct test_treap_node, node)->key;

    if (min && k <= *min) {
        return false;
    }
    if (max && k >= *max) {
        return false;
    }

    /* Heap invariant (min-heap on priority) */
    if (n->left) {
        if (n->left->priority < n->priority || n->left->parent != n) {
            return false;
        }
        if (!verify_treap_invariants(n->left, min, &k)) {
            return false;
        }
    }
    if (n->right) {
        if (n->right->priority < n->priority || n->right->parent != n) {
            return false;
        }
        if (!verify_treap_invariants(n->right, &k, max)) {
            return false;
        }
    }

    return true;
}

TEST_DECLARE_UNIT(treap, basic_operations) {
    struct treap_tree tree;
    treap_tree_init(&tree, &test_treap_ops);

    struct test_treap_node nodes[8];
    int keys[8] = {40, 20, 60, 10, 30, 50, 70, 25};
    uint32_t priorities[8] = {100, 50, 80, 200, 30, 150, 90, 10};

    for (int i = 0; i < 8; i++) {
        nodes[i].key = keys[i];
        treap_init_node(&nodes[i].node, priorities[i]);
        treap_insert(&tree, &nodes[i].node);
        TEST_ASSERT(verify_treap_invariants(tree.root, NULL, NULL));
    }

    /* Verify search */
    for (int i = 0; i < 8; i++) {
        struct treap_node *found = treap_find(&tree, &keys[i]);
        TEST_ASSERT_NONNULL(found);
        TEST_ASSERT_EQ(found, &nodes[i].node);
    }

    int missing = 999;
    TEST_ASSERT_NULL(treap_find(&tree, &missing));

    /* In-order traversal must be strictly ascending */
    struct treap_node *cur = treap_first(&tree);
    int prev = -1;
    int count = 0;
    while (cur) {
        int k = treap_entry(cur, struct test_treap_node, node)->key;
        TEST_ASSERT_GT_S(k, prev);
        prev = k;
        count++;
        cur = treap_next(cur);
    }
    TEST_ASSERT_EQ(count, 8);

    /* Test removal */
    for (int i = 0; i < 8; i++) {
        treap_remove(&tree, &nodes[i].node);
        TEST_ASSERT(verify_treap_invariants(tree.root, NULL, NULL));
        TEST_ASSERT_NULL(treap_find(&tree, &keys[i]));
    }

    TEST_ASSERT(treap_tree_empty(&tree));

    return TEST_SUCCESS;
}
#endif
