/* @title: AVL tree */
#pragma once
#include <container_of.h>
#include <stddef.h>
#include <stdint.h>

struct avl_tree_node {
    int height;
    struct avl_tree_node *left;
    struct avl_tree_node *right;
    struct avl_tree_node *parent;
};

struct avl_tree_node_ops {
    /* Compare two embedded nodes. Returns <0, 0, >0. Required. */
    int (*cmp)(const struct avl_tree_node *a, const struct avl_tree_node *b);
    /* Compare a node against an external key */
    int (*cmp_key)(const struct avl_tree_node *node, const void *key);
};

struct avl_tree {
    struct avl_tree_node *root;
    const struct avl_tree_node_ops *ops;
};

void avl_tree_init(struct avl_tree *tree, const struct avl_tree_node_ops *ops);

void avl_tree_insert(struct avl_tree *tree, struct avl_tree_node *node);

void avl_tree_remove(struct avl_tree *tree, struct avl_tree_node *node);

struct avl_tree_node *avl_tree_find(const struct avl_tree *tree,
                                    const void *key);

struct avl_tree_node *avl_tree_first(const struct avl_tree *tree);
struct avl_tree_node *avl_tree_last(const struct avl_tree *tree);
struct avl_tree_node *avl_tree_next(const struct avl_tree_node *node);
struct avl_tree_node *avl_tree_prev(const struct avl_tree_node *node);

static inline int avl_tree_empty(const struct avl_tree *tree) {
    return tree->root == NULL;
}

#define AVL_NODE_INIT                                                          \
    (struct avl_tree_node) {                                                   \
        .height = 1, .left = NULL, .right = NULL, .parent = NULL               \
    }

static inline void avl_init_node(struct avl_tree_node *n) {
    n->height = 1;
    n->left = n->right = n->parent = NULL;
}

#define avl_entry(ptr, type, member) container_of(ptr, type, member)

#define avl_tree_for_each(pos, tree)                                           \
    for ((pos) = avl_tree_first(tree); (pos); (pos) = avl_tree_next(pos))

#define avl_tree_for_each_safe(pos, n, tree)                                   \
    for ((pos) = avl_tree_first(tree),                                         \
        (n) = (pos) ? avl_tree_next(pos) : NULL;                               \
         (pos); (pos) = (n), (n) = (pos) ? avl_tree_next(pos) : NULL)

#define avl_tree_for_each_entry(pos, type, member, tree)                       \
    for (pos = avl_entry(avl_tree_first(tree), type, member);                  \
         (pos) != NULL && &pos->member != NULL;                                \
         pos = avl_entry(avl_tree_next(&pos->member), type, member))

#define avl_tree_for_each_entry_safe(pos, tmp, type, member, tree)             \
    for (pos = avl_entry(avl_tree_first(tree), type, member),                  \
        tmp = (pos) ? avl_entry(avl_tree_next(&pos->member), type, member)     \
                    : NULL;                                                    \
         (pos) != NULL && &pos->member != NULL; pos = tmp,                     \
        tmp = (pos) ? avl_entry(avl_tree_next(&pos->member), type, member)     \
                    : NULL)

#define avl_tree_for_each_reverse(pos, tree)                                   \
    for ((pos) = avl_tree_last(tree); (pos); (pos) = avl_tree_prev(pos))

#define avl_tree_for_each_safe_reverse(pos, n, tree)                           \
    for ((pos) = avl_tree_last(tree), (n) = (pos) ? avl_tree_prev(pos) : NULL; \
         (pos); (pos) = (n), (n) = (pos) ? avl_tree_prev(pos) : NULL)

#define avl_tree_for_each_entry_reverse(pos, type, member, tree)               \
    for (pos = avl_entry(avl_tree_last(tree), type, member);                   \
         (pos) != NULL && &pos->member != NULL;                                \
         pos = avl_entry(avl_tree_prev(&pos->member), type, member))

#define avl_tree_for_each_entry_safe_reverse(pos, tmp, type, member, tree)     \
    for (pos = avl_entry(avl_tree_last(tree), type, member),                   \
        tmp = (pos) ? avl_entry(avl_tree_prev(&pos->member), type, member)     \
                    : NULL;                                                    \
         (pos) != NULL && &pos->member != NULL; pos = tmp,                     \
        tmp = (pos) ? avl_entry(avl_tree_prev(&pos->member), type, member)     \
                    : NULL)
