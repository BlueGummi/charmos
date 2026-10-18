#include "structures/tests/test_internal.h"

TEST_GROUP_DEFINE(rbt);

struct test_rbt_node {
    int key;
    struct rbt_node node;
};

static size_t test_rbt_get_data(struct rbt_node *node) {
    return (size_t) rbt_entry(node, struct test_rbt_node, node)->key;
}

static int32_t test_rbt_compare(const struct rbt_node *a,
                                const struct rbt_node *b) {
    int a_key = rbt_entry(a, struct test_rbt_node, node)->key;
    int b_key = rbt_entry(b, struct test_rbt_node, node)->key;
    return (a_key > b_key) - (a_key < b_key);
}

static int rbt_black_height(const struct rbt_node *node) {
    if (!node)
        return 1;

    if (rbt_color(node) == TREE_NODE_RED &&
        ((node->left && rbt_color(node->left) == TREE_NODE_RED) ||
         (node->right && rbt_color(node->right) == TREE_NODE_RED)))
        return -1;

    int left_height = rbt_black_height(node->left);
    int right_height = rbt_black_height(node->right);
    if (left_height < 0 || left_height != right_height)
        return -1;

    return left_height + (rbt_color(node) == TREE_NODE_BLACK);
}

static bool verify_rbt_invariants(const struct rbt_node *node, const int *min,
                                  const int *max) {
    if (!node)
        return true;

    int key = rbt_entry(node, struct test_rbt_node, node)->key;
    if ((min && key <= *min) || (max && key >= *max))
        return false;
    if ((node->left && rbt_parent(node->left) != node) ||
        (node->right && rbt_parent(node->right) != node))
        return false;

    return verify_rbt_invariants(node->left, min, &key) &&
           verify_rbt_invariants(node->right, &key, max);
}

TEST_DEFINE_UNIT(rbt, delete_black_leaf_rebalances) {
    struct rbt tree;
    struct test_rbt_node nodes[] = {
        {.key = 8}, {.key = 75}, {.key = 80}, {.key = 31}};
    int expected[] = {8, 31, 75};

    rbt_init(&tree, test_rbt_get_data, test_rbt_compare);
    for (size_t i = 0; i < TEST_ARRAY_LEN(nodes); i++) {
        rbt_init_node(&nodes[i].node);
        rbt_insert(&tree, &nodes[i].node);
    }

    rbt_delete(&tree, &nodes[2].node);

    TEST_ASSERT_NONNULL(tree.root);
    TEST_ASSERT_EQ(rbt_color(tree.root), TREE_NODE_BLACK);
    TEST_ASSERT_GE(rbt_black_height(tree.root), 0);
    TEST_ASSERT(verify_rbt_invariants(tree.root, NULL, NULL));

    size_t index = 0;
    struct rbt_node *node;
    rbt_for_each(node, &tree) {
        TEST_ASSERT_LT(index, TEST_ARRAY_LEN(expected));
        TEST_ASSERT_EQ(rbt_entry(node, struct test_rbt_node, node)->key,
                       expected[index++]);
    }
    TEST_ASSERT_EQ(index, TEST_ARRAY_LEN(expected));
    TEST_ASSERT_NULL(rbt_search(&tree, 80));

    return TEST_SUCCESS;
}
