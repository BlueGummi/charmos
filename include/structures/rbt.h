/* @title: Red black tree */
#pragma once
#include <container_of.h>
#include <enforce.h>
#include <math/align.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rbt_parent {
    uintptr_t word; /* Lowest bit is color */
};

enum rbt_node_color { TREE_NODE_RED = 0, TREE_NODE_BLACK = 1 };

struct rbt_node {
    struct rbt_node *left;
    struct rbt_node *right;
    struct rbt_parent parent_data;
};

typedef int32_t (*rbt_compare)(const struct rbt_node *a,
                               const struct rbt_node *b);
typedef size_t (*rbt_get_data)(struct rbt_node *);

struct rbt { /* TODO: stop using get_data. for now it works
              * but in the future we may want to allow for rb-trees
              * that are "backwards" or sorted by some other rule
              * beyond integer field comparison */
    rbt_get_data get_data;
    rbt_compare compare;
    struct rbt_node *root;
};

#define RBT_NODE_INIT                                                          \
    (struct rbt_node) {                                                        \
        .parent_data.word = TREE_NODE_BLACK, .left = NULL, .right = NULL       \
    }

static inline struct rbt_node *rbt_parent(const struct rbt_node *node) {
    return (struct rbt_node *) (node->parent_data.word & ~1ULL);
}

static inline enum rbt_node_color rbt_color(const struct rbt_node *node) {
    return (enum rbt_node_color)(node->parent_data.word & 1ULL);
}

static inline void rbt_set_color(struct rbt_node *node, enum rbt_node_color c) {
    enforce(c == TREE_NODE_BLACK || c == TREE_NODE_RED);
    node->parent_data.word &= ~1ULL;
    node->parent_data.word |= c;
}

static inline void rbt_set_parent(struct rbt_node *node,
                                  struct rbt_node *parent) {
    /* Lowest bit cannot be set */
    enforce(((uintptr_t) parent & 1ULL) == 0);

    node->parent_data.word &= 1ULL;
    node->parent_data.word |= (uintptr_t) parent;
}

static inline struct rbt_node *rbt_last(const struct rbt *root) {
    struct rbt_node *node = root->root;
    if (!node)
        return NULL;
    while (node->right)
        node = node->right;
    return node;
}

static inline struct rbt_node *rbt_prev(struct rbt_node *node) {
    if (node->left) {
        /* predecessor is rightmost node of left subtree */
        node = node->left;
        while (node->right)
            node = node->right;
        return node;
    }

    /* climb up until we come from the right */

    struct rbt_node *parent = rbt_parent(node);
    while (parent && node == parent->left) {
        node = parent;
        parent = rbt_parent(parent);
    }
    return parent;
}

static inline void rbt_init_node(struct rbt_node *n) {
    n->parent_data.word = TREE_NODE_BLACK;
    n->left = n->right = NULL;
}

static inline struct rbt_node *rbt_first(const struct rbt *root) {
    struct rbt_node *node = root->root;
    if (!node)
        return NULL;
    while (node->left)
        node = node->left;
    return node;
}

void rbt_link_node(struct rbt_node *node, struct rbt_node *parent,
                   struct rbt_node **link);
void rbt_insert_color(struct rbt *tree, struct rbt_node *node);
struct rbt *rbt_init(struct rbt *t, rbt_get_data get_data, rbt_compare compare);
struct rbt *rbt_create(rbt_get_data get, rbt_compare compare);
struct rbt_node *rbt_find_min(struct rbt_node *node);
struct rbt_node *rbt_find_max(struct rbt_node *node);

/* `z` must be linked into `tree` */
void rbt_delete(struct rbt *tree, struct rbt_node *z);
struct rbt_node *rbt_search(struct rbt *tree, uint64_t data);
void rbt_remove(struct rbt *tree, uint64_t data);
void rbt_insert(struct rbt *tree, struct rbt_node *new_node);
struct rbt_node *rbt_min(struct rbt *tree);
struct rbt_node *rbt_max(struct rbt *tree);
struct rbt_node *rbt_next(struct rbt_node *node);
struct rbt_node *rbt_find_predecessor(struct rbt *tree, uint64_t data);
struct rbt_node *rbt_find_successor(struct rbt *tree, uint64_t data);

static inline bool rbt_empty(struct rbt *tree) {
    return !tree->root;
}

bool rbt_has_node(struct rbt *tree, struct rbt_node *node);

/* Re-position nodes whose sort key changes, with checks
 * to first verify it needs to move at all, returning true if moved */
bool rbt_reinsert(struct rbt *tree, struct rbt_node *node);

/* True when ordering is correct */
bool rbt_position_valid(struct rbt *tree, struct rbt_node *node);

void rbt_move(struct rbt *from, struct rbt *to, struct rbt_node *node);

#define rbt_for_each_safe(pos, tmp, root)                                      \
    for (pos = rbt_first(root), tmp = rbt_next(pos); pos != NULL;              \
         pos = tmp, tmp = rbt_next(pos))

#define rbt_for_each_entry_safe(pos, tmp, tree, member)                        \
    for (typeof(rbt_first(tree)) __n = rbt_first(tree), __nx = NULL;           \
         __n != NULL &&                                                        \
         (((pos) = rbt_entry(__n, typeof(*(pos)), member)),                    \
          (__nx = rbt_next(__n)),                                              \
          ((tmp) = __nx ? rbt_entry(__nx, typeof(*(tmp)), member) : NULL),     \
          true);                                                               \
         __n = __nx)

#define rbt_for_each_safe_reverse(pos, tmp, root)                              \
    for (pos = rbt_last(root), tmp = rbt_prev(pos); pos != NULL;               \
         pos = tmp, tmp = rbt_prev(pos))

#define rbt_for_each_entry_safe_reverse(pos, tmp, tree, member)                \
    for (typeof(rbt_last(tree)) __n = rbt_last(tree), __nx = NULL;             \
         __n != NULL &&                                                        \
         (((pos) = rbt_entry(__n, typeof(*(pos)), member)),                    \
          (__nx = rbt_prev(__n)),                                              \
          ((tmp) = __nx ? rbt_entry(__nx, typeof(*(tmp)), member) : NULL),     \
          true);                                                               \
         __n = __nx)

#define rbt_for_each(pos, root)                                                \
    for (pos = rbt_first(root); pos != NULL; pos = rbt_next(pos))

#define rbt_for_each_entry(pos, tree, member)                                  \
    for (typeof(rbt_first(tree)) __n = rbt_first(tree);                        \
         __n != NULL &&                                                        \
         (((pos) = rbt_entry(__n, typeof(*(pos)), member)), true);             \
         __n = rbt_next(__n))

#define rbt_for_each_reverse(pos, root)                                        \
    for (pos = rbt_last(root); pos != NULL; pos = rbt_prev(pos))

#define rbt_for_each_entry_reverse(pos, tree, member)                          \
    for (typeof(rbt_last(tree)) __n = rbt_last(tree);                          \
         __n != NULL &&                                                        \
         (((pos) = rbt_entry(__n, typeof(*(pos)), member)), true);             \
         __n = rbt_prev(__n))

#define rbt_entry(ptr, type, member) container_of(ptr, type, member)
