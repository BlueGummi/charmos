/* @title: Treap (Randomized Binary Search Tree) */
#pragma once
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct treap_node {
    uint32_t priority;
    struct treap_node *left;
    struct treap_node *right;
    struct treap_node *parent;
};

struct treap_node_ops {
    int (*cmp)(const struct treap_node *a, const struct treap_node *b);
    int (*cmp_key)(const struct treap_node *node, const void *key);
};

struct treap_tree {
    struct treap_node *root;
    const struct treap_node_ops *ops;
};

#define TREAP_NODE_INIT(prio)                                                  \
    (struct treap_node) {                                                      \
        .priority = (prio), .left = NULL, .right = NULL, .parent = NULL        \
    }

static inline void treap_init_node(struct treap_node *n, uint32_t priority) {
    n->priority = priority;
    n->left = n->right = n->parent = NULL;
}

static inline bool treap_tree_empty(const struct treap_tree *tree) {
    return tree->root == NULL;
}

void treap_tree_init(struct treap_tree *tree, const struct treap_node_ops *ops);

void treap_insert(struct treap_tree *tree, struct treap_node *node);

void treap_remove(struct treap_tree *tree, struct treap_node *node);

struct treap_node *treap_find(const struct treap_tree *tree, const void *key);

struct treap_node *treap_first(const struct treap_tree *tree);
struct treap_node *treap_last(const struct treap_tree *tree);
struct treap_node *treap_next(const struct treap_node *node);
struct treap_node *treap_prev(const struct treap_node *node);

#define treap_entry(ptr, type, member) container_of(ptr, type, member)

#define treap_for_each(pos, tree)                                              \
    for ((pos) = treap_first(tree); (pos); (pos) = treap_next(pos))

#define treap_for_each_safe(pos, n, tree)                                      \
    for ((pos) = treap_first(tree), (n) = (pos) ? treap_next(pos) : NULL;      \
         (pos); (pos) = (n), (n) = (pos) ? treap_next(pos) : NULL)

#define treap_for_each_entry(pos, type, member, tree)                          \
    for (pos = treap_entry(treap_first(tree), type, member);                   \
         (pos) != NULL && &pos->member != NULL;                                \
         pos = treap_entry(treap_next(&pos->member), type, member))

#define treap_for_each_entry_safe(pos, tmp, type, member, tree)                \
    for (pos = treap_entry(treap_first(tree), type, member),                   \
        tmp = (pos) ? treap_entry(treap_next(&pos->member), type, member)      \
                    : NULL;                                                    \
         (pos) != NULL && &pos->member != NULL; pos = tmp,                     \
        tmp = (pos) ? treap_entry(treap_next(&pos->member), type, member)      \
                    : NULL)

#define treap_for_each_reverse(pos, tree)                                      \
    for ((pos) = treap_last(tree); (pos); (pos) = treap_prev(pos))

#define treap_for_each_safe_reverse(pos, n, tree)                              \
    for ((pos) = treap_last(tree), (n) = (pos) ? treap_prev(pos) : NULL;       \
         (pos); (pos) = (n), (n) = (pos) ? treap_prev(pos) : NULL)

#define treap_for_each_entry_reverse(pos, type, member, tree)                  \
    for (pos = treap_entry(treap_last(tree), type, member);                    \
         (pos) != NULL && &pos->member != NULL;                                \
         pos = treap_entry(treap_prev(&pos->member), type, member))

#define treap_for_each_entry_safe_reverse(pos, tmp, type, member, tree)        \
    for (pos = treap_entry(treap_last(tree), type, member),                    \
        tmp = (pos) ? treap_entry(treap_prev(&pos->member), type, member)      \
                    : NULL;                                                    \
         (pos) != NULL && &pos->member != NULL; pos = tmp,                     \
        tmp = (pos) ? treap_entry(treap_prev(&pos->member), type, member)      \
                    : NULL)
