#ifdef TEST_RBIT

#include <crypto/prng.h>
#include <mem/alloc.h>
#include <structures/rbit.h>
#include <test.h>

TEST_GROUP_DECLARE(rbit);

#define RBIT_N 256
#define RBIT_OPS 4000
#define RBIT_SEED 0xC0FFEEULL

static size_t subtree_nodes(struct rbit_node *n) {
    if (!n)
        return 0;
    return 1 + subtree_nodes(n->left) + subtree_nodes(n->right);
}

static int overlaps(struct interval a, struct interval b) {
    return a.low <= b.high && b.low <= a.high;
}

TEST_DECLARE(rbit_order_search, .tier = TEST_TIER_UNIT,
             .group = TEST_GROUP(rbit)) {
    prng_seed(RBIT_SEED);
    struct rbit tree;
    rbit_init(&tree);

    struct rbit_node *nodes =
        kmalloc(sizeof(*nodes) * RBIT_N, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(nodes);

    size_t low = 1;
    for (size_t i = 0; i < RBIT_N; i++) {
        low += 1 + (prng_next() % 64);
        rbit_init_node(&nodes[i]);
        nodes[i].interval.low = low;
        nodes[i].interval.high = low + (prng_next() % 32);
        rbit_insert(&tree, &nodes[i]);
    }

    size_t prev = 0;
    size_t count = 0;
    struct rbit_node *it;
    rbit_for_each(it, &tree) {
        TEST_ASSERT(it->interval.low >= prev);
        prev = it->interval.low;
        count++;
    }
    TEST_ASSERT(count == RBIT_N);

    for (size_t i = 0; i < RBIT_N; i++)
        TEST_ASSERT(rbit_search(tree.root, nodes[i].interval) == &nodes[i]);

    for (size_t i = 0; i < RBIT_N; i += 2)
        rbit_delete(&tree, &nodes[i]);
    for (size_t i = 0; i < RBIT_N; i++) {
        struct rbit_node *f = rbit_search(tree.root, nodes[i].interval);
        TEST_ASSERT((i % 2 == 0) ? (f == NULL) : (f == &nodes[i]));
    }

    for (size_t i = 1; i < RBIT_N; i += 2)
        rbit_delete(&tree, &nodes[i]);
    TEST_ASSERT(rbit_empty(&tree));

    return TEST_SUCCESS;
}

TEST_DECLARE(rbit_overlap, .tier = TEST_TIER_UNIT, .group = TEST_GROUP(rbit)) {
    prng_seed(RBIT_SEED + 1);
    struct rbit tree;
    rbit_init(&tree);

    struct rbit_node *nodes =
        kmalloc(sizeof(*nodes) * RBIT_N, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(nodes);

    /* Random (possibly overlapping) intervals in a bounded space. */
    for (size_t i = 0; i < RBIT_N; i++) {
        size_t lo = prng_next() % 100000;
        rbit_init_node(&nodes[i]);
        nodes[i].interval.low = lo;
        nodes[i].interval.high = lo + (prng_next() % 500);
        rbit_insert(&tree, &nodes[i]);
    }

    for (size_t q = 0; q < RBIT_OPS; q++) {
        size_t lo = prng_next() % 100000;
        struct interval iv = {.low = lo, .high = lo + (prng_next() % 500)};

        bool brute = false;
        for (size_t i = 0; i < RBIT_N; i++)
            if (overlaps(nodes[i].interval, iv)) {
                brute = true;
                break;
            }

        struct rbit_node *res = rbit_overlap_search(tree.root, iv);
        TEST_ASSERT((res != NULL) == brute);
        if (res)
            TEST_ASSERT(overlaps(res->interval, iv));
    }

    return TEST_SUCCESS;
}

struct count_node {
    struct rbit_node node;
    size_t subtree_count; /* maintained by the augment below */
};

static size_t cn_count(struct rbit_node *n) {
    return n ? rbit_entry(n, struct count_node, node)->subtree_count : 0;
}

static bool count_augment(struct rbit_node *n) {
    struct count_node *c = rbit_entry(n, struct count_node, node);
    size_t old_max = n->max, old_cnt = c->subtree_count;

    size_t mx = n->interval.high;
    if (rbit_node_max(n->left) > mx)
        mx = rbit_node_max(n->left);
    if (rbit_node_max(n->right) > mx)
        mx = rbit_node_max(n->right);

    n->max = mx;
    c->subtree_count = 1 + cn_count(n->left) + cn_count(n->right);
    return n->max != old_max || c->subtree_count != old_cnt;
}

TEST_DECLARE(rbit_augment_hook, .tier = TEST_TIER_UNIT,
             .group = TEST_GROUP(rbit)) {
    prng_seed(RBIT_SEED + 2);
    struct rbit tree;
    rbit_init(&tree);
    tree.augment = count_augment;

    struct count_node *nodes =
        kmalloc(sizeof(*nodes) * RBIT_N, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(nodes);
    bool *live = kmalloc(sizeof(bool) * RBIT_N, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(live);

    for (size_t i = 0; i < RBIT_N; i++) {
        rbit_init_node(&nodes[i].node);
        nodes[i].node.interval.low = i * 100 + 1;
        nodes[i].node.interval.high = i * 100 + 50;
    }

    for (size_t op = 0; op < RBIT_OPS; op++) {
        size_t i = prng_next() % RBIT_N;
        if (live[i]) {
            rbit_delete(&tree, &nodes[i].node);
            live[i] = false;
        } else {
            rbit_insert(&tree, &nodes[i].node);
            live[i] = true;
        }

        struct rbit_node *it;
        rbit_for_each(it, &tree) {
            struct count_node *c = rbit_entry(it, struct count_node, node);
            TEST_ASSERT(c->subtree_count == subtree_nodes(it));
        }
    }

    return TEST_SUCCESS;
}

#endif
