#pragma once
#include <misc/containerof.h>
#include <stddef.h>
#include <stdint.h>

#define rbt_for_each(pos, root)                                                \
    for (pos = rb_first(root); pos != NULL; pos = rbt_next(pos))

#define rbt_entry(ptr, type, member) container_of(ptr, type, member)
#define rbt_parent(n) ((n)->parent)
#define RBTREE_COMPARE(a, b) ((a)->key - (b)->key)

enum rbt_node_color { TREE_NODE_RED, TREE_NODE_BLACK };

struct rbt_node {
    uint64_t data;
    enum rbt_node_color color;
    struct rbt_node *left;
    struct rbt_node *right;
    struct rbt_node *parent;
};

struct rbt {
    struct rbt_node *root;
};

static inline void rbt_init_node(struct rbt_node *n) {
    n->color = TREE_NODE_BLACK;
    n->left = n->right = n->parent = NULL;
}

static inline struct rbt_node *rb_first(const struct rbt *root) {
    struct rbt_node *node = root->root;
    if (!node)
        return NULL;
    while (node->left)
        node = node->left;
    return node;
}

struct rbt *rbt_create(void);
struct rbt_node *rbt_find_min(struct rbt_node *node);
void rb_delete(struct rbt *tree, struct rbt_node *z);
struct rbt_node *rbt_search(struct rbt_node *root, uint64_t data);
void rbt_remove(struct rbt *tree, uint64_t data);
void rbt_insert(struct rbt *tree, struct rbt_node *new_node);
struct rbt_node *rbt_min(struct rbt *tree);
struct rbt_node *rbt_next(struct rbt_node *node);
