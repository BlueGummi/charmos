#include <console/panic.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/radix.h>

static bool radix_verify_tree(struct radix_tree *tree);
static void radix_prune_up(struct radix_node *node, struct radix_tree *tree);

static inline uint64_t radix_index(uint64_t key, uint32_t level) {
    uint32_t shift = level * RADIX_BITS;
    return (key >> shift) & RADIX_MASK;
}

int32_t radix_insert(struct radix_tree *tree, void *item) {
    uint64_t key = tree->key_fn(item);
    int32_t level = tree->height;

    if (!tree->root) {
        tree->root = kmalloc(sizeof(struct radix_node), ALLOC_FLAGS_ZERO);
        if (!tree->root)
            return ERR_NO_MEM;
    }

    struct radix_node *node = tree->root;

    for (; level > 1; level--) {
        uint64_t idx = radix_index(key, level - 1);

        if (!node->slots[idx]) {
            struct radix_node *mid =
                kmalloc(sizeof(struct radix_node), ALLOC_FLAGS_ZERO);
            if (!mid) {
                radix_prune_up(node, tree);
                return ERR_NO_MEM;
            }
            mid->parent = node;
            node->slots[idx] = mid;
            node->present_mask |= (1ULL << idx);
        }

        node = node->slots[idx];
    }

    uint64_t idx = radix_index(key, 0);
    if (node->slots[idx]) {
        radix_verify_tree(tree);
        return ERR_EXIST;
    }

    node->slots[idx] = item;
    node->present_mask |= (1ULL << idx);
    radix_verify_tree(tree);

    return 0;
}

void *radix_lookup(struct radix_tree *tree, uint64_t key) {
    struct radix_node *node = tree->root;
    for (int32_t level = tree->height; level > 1; level--) {
        if (!node)
            return NULL;
        uint64_t idx = radix_index(key, level - 1);
        node = node->slots[idx];
    }
    if (!node)
        return NULL;
    return node->slots[radix_index(key, 0)];
}

static bool radix_verify_node(struct radix_tree *tree, struct radix_node *node,
                              struct radix_node *expected_parent, int level,
                              int max_height, uint64_t prefix,
                              int *node_count) {
    if (!node)
        return true;

    if (node->parent != expected_parent)
        panic("Node %p has incorrect parent %p (expected %p)", (void *) node,
              (void *) node->parent, (void *) expected_parent);

    if (node_count)
        (*node_count)++;

    uint64_t expected_mask = 0;
    for (int i = 0; i < RADIX_SIZE; i++) {
        if (node->slots[i])
            expected_mask |= (1ULL << i);
    }

    if (expected_mask != node->present_mask)
        panic("Node %p present_mask mismatch: expected 0x%llx, got 0x%llx",
              (void *) node, (unsigned long long) expected_mask,
              (unsigned long long) node->present_mask);

    bool is_leaf_parent = (level + 1 == max_height);

    for (int i = 0; i < RADIX_SIZE; i++) {
        void *child = node->slots[i];
        if (!child)
            continue;

        if (level >= max_height)
            panic("Node %p at level %d has child beyond max height %d",
                  (void *) node, level, max_height);

        uint32_t shift = (max_height - level - 1) * RADIX_BITS;
        uint64_t child_prefix = prefix | ((uint64_t) i << shift);

        if (is_leaf_parent) {
            uint64_t item_key = tree->key_fn(child);
            if (item_key != child_prefix)
                panic("Leaf item at slot %d has key %llu (expected %llu)", i,
                      (unsigned long long) item_key,
                      (unsigned long long) child_prefix);

            if (node_count)
                (*node_count)++;
        } else if (!radix_verify_node(tree, child, node, level + 1, max_height,
                                      child_prefix, node_count)) {
            return false;
        }
    }

    return true;
}

static bool radix_verify_tree(struct radix_tree *tree) {
    if (!tree || !tree->root)
        return true;

    int node_count = 0;
    return radix_verify_node(tree, tree->root, NULL, 0, tree->height, 0,
                             &node_count);
}

static void radix_prune_up(struct radix_node *node, struct radix_tree *tree) {
    while (node && node->present_mask == 0) {
        struct radix_node *parent = node->parent;
        if (!parent) {
            if (tree->root == node) {
                kfree(node);
                tree->root = NULL;
            }
            break;
        }

        for (uint64_t i = 0; i < RADIX_SIZE; i++) {
            if (parent->slots[i] == node) {
                parent->slots[i] = NULL;
                parent->present_mask &= ~(1ULL << i);
                break;
            }
        }

        kfree(node);
        node = parent;
    }
}

void *radix_delete(struct radix_tree *tree, uint64_t key) {
    struct radix_node *node = tree->root;
    uint64_t idx = 0;

    for (int32_t level = tree->height; level > 1; level--) {
        if (!node)
            return NULL;
        idx = radix_index(key, level - 1);
        node = node->slots[idx];
    }

    if (!node)
        return NULL;

    idx = radix_index(key, 0);
    void *item = node->slots[idx];
    if (!item)
        return NULL;

    node->slots[idx] = NULL;
    node->present_mask &= ~(1ULL << idx);

    radix_prune_up(node, tree);
    radix_verify_tree(tree);

    return item;
}

struct radix_tree *radix_tree_init(struct radix_tree *r, radix_key_fn kfn,
                                   int height) {
    r->root = NULL;
    r->key_fn = kfn;
    r->height = height;
    return r;
}
