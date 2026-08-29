#include "structures/tests/test_internal.h"

#ifdef TEST_SPLAY
TEST_GROUP_DECLARE(splay);

struct test_splay_node {
    int key;
    struct splay_node node;
};

static int test_splay_cmp(const struct splay_node *a,
                          const struct splay_node *b) {
    int ka = splay_entry(a, struct test_splay_node, node)->key;
    int kb = splay_entry(b, struct test_splay_node, node)->key;
    return (ka > kb) - (ka < kb);
}

static int test_splay_cmp_key(const struct splay_node *a, const void *key) {
    int ka = splay_entry(a, struct test_splay_node, node)->key;
    int kk = *(const int *) key;
    return (ka > kk) - (ka < kk);
}

static const struct splay_node_ops test_splay_ops = {
    .cmp = test_splay_cmp,
    .cmp_key = test_splay_cmp_key,
};

static bool verify_splay_bst_invariants(struct splay_node *n, int *min,
                                        int *max) {
    if (!n) {
        return true;
    }

    int k = splay_entry(n, struct test_splay_node, node)->key;

    if (min && k <= *min) {
        return false;
    }
    if (max && k >= *max) {
        return false;
    }

    if (n->left) {
        if (n->left->parent != n) {
            return false;
        }
        if (!verify_splay_bst_invariants(n->left, min, &k)) {
            return false;
        }
    }
    if (n->right) {
        if (n->right->parent != n) {
            return false;
        }
        if (!verify_splay_bst_invariants(n->right, &k, max)) {
            return false;
        }
    }

    return true;
}

TEST_DECLARE_UNIT(splay, basic_operations) {
    struct splay_tree tree;
    splay_tree_init(&tree, &test_splay_ops);

    struct test_splay_node nodes[8];
    int keys[8] = {50, 20, 70, 10, 30, 60, 80, 25};

    for (int i = 0; i < 8; i++) {
        nodes[i].key = keys[i];
        splay_insert(&tree, &nodes[i].node);
        TEST_ASSERT(verify_splay_bst_invariants(tree.root, NULL, NULL));
        /* The newly inserted node should be at the root */
        TEST_ASSERT_EQ(tree.root, &nodes[i].node);
    }

    /* Verify search & splay-to-root property */
    for (int i = 0; i < 8; i++) {
        struct splay_node *found = splay_find(&tree, &keys[i]);
        TEST_ASSERT_NONNULL(found);
        TEST_ASSERT_EQ(found, &nodes[i].node);
        TEST_ASSERT_EQ(tree.root, found);
        TEST_ASSERT(verify_splay_bst_invariants(tree.root, NULL, NULL));
    }

    int missing = 999;
    TEST_ASSERT_NULL(splay_find(&tree, &missing));

    /* In-order traversal must be ascending */
    struct splay_node *cur = splay_first(&tree);
    int prev = -1;
    int count = 0;
    while (cur) {
        int k = splay_entry(cur, struct test_splay_node, node)->key;
        TEST_ASSERT_GT_S(k, prev);
        prev = k;
        count++;
        cur = splay_next(cur);
    }
    TEST_ASSERT_EQ(count, 8);

    /* Test removal */
    for (int i = 0; i < 8; i++) {
        splay_remove(&tree, &nodes[i].node);
        TEST_ASSERT(verify_splay_bst_invariants(tree.root, NULL, NULL));
        TEST_ASSERT_NULL(splay_find(&tree, &keys[i]));
    }

    TEST_ASSERT(splay_tree_empty(&tree));

    return TEST_SUCCESS;
}
#endif
