#include "../test_internal.h"

#ifdef TEST_AVL
TEST_GROUP_DECLARE(avl);

struct test_avl_node {
    int key;
    struct avl_tree_node node;
};

static int test_avl_cmp(const struct avl_tree_node *a,
                        const struct avl_tree_node *b) {
    int ka = avl_entry(a, struct test_avl_node, node)->key;
    int kb = avl_entry(b, struct test_avl_node, node)->key;
    return (ka > kb) - (ka < kb);
}

static int test_avl_cmp_key(const struct avl_tree_node *a, const void *key) {
    int ka = avl_entry(a, struct test_avl_node, node)->key;
    int kk = *(const int *) key;
    return (ka > kk) - (ka < kk);
}

static const struct avl_tree_node_ops test_avl_ops = {
    .cmp = test_avl_cmp,
    .cmp_key = test_avl_cmp_key,
};

static int node_height(struct avl_tree_node *n) {
    if (!n)
        return 0;
    int lh = node_height(n->left);
    int rh = node_height(n->right);
    return (lh > rh ? lh : rh) + 1;
}

static bool verify_avl_invariants(struct avl_tree_node *n) {
    if (!n)
        return true;

    int lh = node_height(n->left);
    int rh = node_height(n->right);
    int diff = lh - rh;
    if (diff < -1 || diff > 1)
        return false;

    if (n->height != (lh > rh ? lh : rh) + 1)
        return false;

    if (n->left && n->left->parent != n)
        return false;
    if (n->right && n->right->parent != n)
        return false;

    return verify_avl_invariants(n->left) && verify_avl_invariants(n->right);
}

TEST_DECLARE_UNIT(avl_tree_rotations_and_balance, .group = TEST_GROUP(avl)) {
    struct avl_tree tree;
    avl_tree_init(&tree, &test_avl_ops);

    struct test_avl_node nodes[7];
    int insert_keys[7] = {30, 20, 40, 10, 25, 35, 50};

    for (int i = 0; i < 7; i++) {
        nodes[i].key = insert_keys[i];
        avl_tree_insert(&tree, &nodes[i].node);
        TEST_ASSERT(verify_avl_invariants(tree.root));
    }

    /* Verify search */
    for (int i = 0; i < 7; i++) {
        struct avl_tree_node *found = avl_tree_find(&tree, &insert_keys[i]);
        TEST_ASSERT(found != NULL);
        TEST_ASSERT(avl_entry(found, struct test_avl_node, node)->key ==
                    insert_keys[i]);
    }

    int missing = 999;
    TEST_ASSERT(avl_tree_find(&tree, &missing) == NULL);

    /* In-order traversal must be ascending */
    struct avl_tree_node *cur = avl_tree_first(&tree);
    int prev_key = -1;
    int count = 0;
    while (cur) {
        int k = avl_entry(cur, struct test_avl_node, node)->key;
        TEST_ASSERT(k > prev_key);
        prev_key = k;
        count++;
        cur = avl_tree_next(cur);
    }
    TEST_ASSERT(count == 7);

    /* removal with successor transplant */
    for (int i = 0; i < 7; i++) {
        avl_tree_remove(&tree, &nodes[i].node);
        TEST_ASSERT(verify_avl_invariants(tree.root));
        TEST_ASSERT(avl_tree_find(&tree, &insert_keys[i]) == NULL);
    }

    TEST_ASSERT(avl_tree_empty(&tree));

    return TEST_SUCCESS;
}
#endif
