#include "../test_internal.h"

#ifdef TEST_RADIX
TEST_GROUP_DECLARE(radix);

struct test_item {
    uint64_t key;
    uint64_t val;
};

static uint64_t test_item_key(const void *item) {
    return ((const struct test_item *) item)->key;
}

TEST_DECLARE_UNIT(radix, radix_tree_insert_lookup_delete) {
    struct radix_tree tree;
    radix_tree_init(&tree, test_item_key, 2);

    struct test_item items[6] = {
        {.key = 0, .val = 100},     {.key = 1, .val = 101},
        {.key = 63, .val = 163},    {.key = 64, .val = 164},
        {.key = 4095, .val = 4095}, {.key = 250, .val = 1250},
    };

    for (size_t i = 0; i < 6; i++)
        TEST_ASSERT_EQ(radix_insert(&tree, &items[i]), 0);

    /* Duplicate insert must return ERR_EXIST without clobbering slots. */
    struct test_item dup = {.key = 64, .val = 999};
    TEST_ASSERT_EQ_S(radix_insert(&tree, &dup), ERR_EXIST);

    for (size_t i = 0; i < 6; i++) {
        struct test_item *found = radix_lookup(&tree, items[i].key);
        TEST_ASSERT_NONNULL(found);
        TEST_ASSERT_EQ(found->val, items[i].val);
    }

    TEST_ASSERT_NULL(radix_lookup(&tree, 2));
    TEST_ASSERT_NULL(radix_lookup(&tree, 65));

    /* Deleting must return the original pointer */
    for (size_t i = 0; i < 6; i++) {
        struct test_item *del = radix_delete(&tree, items[i].key);
        TEST_ASSERT_PTR_EQ(del, &items[i]);
        TEST_ASSERT_NULL(radix_lookup(&tree, items[i].key));
    }

    /* Prune up must free empty nodes and zero root when empty */
    TEST_ASSERT_NULL(tree.root);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(radix, radix_tree_multi_level_sparse) {
    struct radix_tree tree;
    radix_tree_init(&tree, test_item_key, 3);

    struct test_item items[4] = {
        {.key = 0, .val = 10},
        {.key = 1000, .val = 20},
        {.key = 50000, .val = 30},
        {.key = 200000, .val = 40},
    };

    for (size_t i = 0; i < 4; i++)
        TEST_ASSERT_EQ(radix_insert(&tree, &items[i]), 0);

    for (size_t i = 0; i < 4; i++) {
        struct test_item *found = radix_lookup(&tree, items[i].key);
        TEST_ASSERT_PTR_EQ(found, &items[i]);
    }

    for (size_t i = 0; i < 4; i++)
        TEST_ASSERT_PTR_EQ(radix_delete(&tree, items[i].key), &items[i]);

    TEST_ASSERT_NULL(tree.root);

    return TEST_SUCCESS;
}
#endif
