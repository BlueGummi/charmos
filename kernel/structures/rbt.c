#include <console/panic.h>
#include <console/printf.h>
#include <enforce.h>
#include <mem/alloc.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/rbt.h>

#ifdef DEBUG_RBT

#define RBT_DEBUG_MAX_DEPTH 128

static inline void rbt_debug_step(size_t *steps, const struct rbt_node *at,
                                  const char *caller) {
    if (cc_unlikely(++*steps > RBT_DEBUG_MAX_DEPTH))
        panic("%s: walk passed %d steps at node %p -- cycle?", caller,
              RBT_DEBUG_MAX_DEPTH, at);
}

static void rbt_debug_check_linked(const struct rbt *tree,
                                   const struct rbt_node *node,
                                   const char *caller) {
    const struct rbt_node *n = node;
    size_t steps = 0;

    while (n->parent) {
        const struct rbt_node *p = n->parent;
        if (cc_unlikely(p->left != n && p->right != n))
            panic("%s: node %p (from %p) claims parent %p, whose children are "
                  "%p/%p, tree %p",
                  caller, n, node, p, p->left, p->right, tree);

        n = p;
        rbt_debug_step(&steps, n, caller);
    }

    if (cc_unlikely(n != tree->root))
        panic("%s: node %p is under root %p, not tree %p (root %p)", caller,
              node, n, tree, tree->root);
}

#else

static inline void rbt_debug_step(size_t *steps, const struct rbt_node *at,
                                  const char *caller) {
    cc_ignore(steps, at, caller);
}

static inline void rbt_debug_check_linked(const struct rbt *tree,
                                          const struct rbt_node *node,
                                          const char *caller) {
    cc_ignore(tree, node, caller);
}

#endif /* DEBUG_RBT */

#ifdef DEBUG_RBT_DEEP

static size_t rbt_debug_validate_subtree(const struct rbt *tree,
                                         const struct rbt_node *node,
                                         size_t depth, const char *caller) {
    if (!node)
        return 1;

    if (cc_unlikely(depth > RBT_DEBUG_MAX_DEPTH))
        panic("%s: tree %p deeper than %d at node %p -- cycle?", caller, tree,
              RBT_DEBUG_MAX_DEPTH, node);

    if (cc_unlikely(node->left && node->left == node->right))
        panic("%s: node %p has %p as both children, tree %p", caller, node,
              node->left, tree);

    if (cc_unlikely(node->left && node->left->parent != node))
        panic("%s: node %p left child %p has parent %p, tree %p", caller, node,
              node->left, node->left->parent, tree);

    if (cc_unlikely(node->right && node->right->parent != node))
        panic("%s: node %p right child %p has parent %p, tree %p", caller, node,
              node->right, node->right->parent, tree);

    if (cc_unlikely(node->color == TREE_NODE_RED &&
                    ((node->left && node->left->color == TREE_NODE_RED) ||
                     (node->right && node->right->color == TREE_NODE_RED))))
        panic("%s: red-red violation at node %p, tree %p", caller, node, tree);

    size_t lh = rbt_debug_validate_subtree(tree, node->left, depth + 1, caller);
    size_t rh =
        rbt_debug_validate_subtree(tree, node->right, depth + 1, caller);

    if (cc_unlikely(lh != rh))
        panic("%s: black-height violation at node %p (left %zu, right %zu), "
              "tree %p",
              caller, node, lh, rh, tree);

    return lh + (node->color == TREE_NODE_BLACK ? 1 : 0);
}

static void rbt_debug_validate(const struct rbt *tree, const char *caller) {
    const struct rbt_node *root = tree->root;
    if (!root)
        return;

    if (cc_unlikely(root->parent))
        panic("%s: root %p of tree %p has parent %p", caller, root, tree,
              root->parent);

    if (cc_unlikely(root->color != TREE_NODE_BLACK))
        panic("%s: root %p of tree %p is red", caller, root, tree);

    rbt_debug_validate_subtree(tree, root, 0, caller);
}

#else

static inline void rbt_debug_validate(const struct rbt *tree,
                                      const char *caller) {
    cc_ignore(tree, caller);
}

#endif /* DEBUG_RBT_DEEP */

struct rbt *rbt_create(rbt_get_data get, rbt_compare cmp) {
    struct rbt *tree = vnew(tree);
    if (!tree)
        return NULL;

    tree->compare = cmp;
    tree->get_data = get;
    tree->root = NULL;
    return tree;
}

struct rbt_node *rbt_find_min(struct rbt_node *node) {
    size_t steps = 0;
    while (node && node->left != NULL) {
        node = node->left;
        rbt_debug_step(&steps, node, __func__);
    }

    return node;
}

struct rbt_node *rbt_find_max(struct rbt_node *node) {
    size_t steps = 0;
    while (node && node->right != NULL) {
        node = node->right;
        rbt_debug_step(&steps, node, __func__);
    }

    return node;
}

struct rbt_node *rbt_max(struct rbt *tree) {
    return rbt_find_max(tree->root);
}

struct rbt_node *rbt_min(struct rbt *tree) {
    return rbt_find_min(tree->root);
}

struct rbt_node *rbt_next(struct rbt_node *node) {
    if (!node)
        return NULL;

    if (node->right)
        return rbt_find_min(node->right);

    size_t steps = 0;
    struct rbt_node *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
        rbt_debug_step(&steps, node, __func__);
    }
    return parent;
}

struct rbt_node *rbt_find_predecessor(struct rbt *tree, uint64_t data) {
    struct rbt_node *curr = tree->root;
    struct rbt_node *pred = NULL;

    while (curr) {
        uint64_t curr_data = tree->get_data(curr);
        if (curr_data < data) {
            pred = curr;
            curr = curr->right;
        } else {
            curr = curr->left;
        }
    }
    return pred;
}

struct rbt_node *rbt_find_successor(struct rbt *tree, uint64_t data) {
    struct rbt_node *curr = tree->root;
    struct rbt_node *succ = NULL;

    while (curr) {
        uint64_t curr_data = tree->get_data(curr);
        if (curr_data > data) {
            succ = curr;
            curr = curr->left;
        } else {
            curr = curr->right;
        }
    }
    return succ;
}

static void rb_transplant(struct rbt *tree, struct rbt_node *u,
                          struct rbt_node *v) {
    if (u->parent == NULL)
        tree->root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;

    if (v)
        v->parent = u->parent;
}

static void left_rotate(struct rbt *tree, struct rbt_node *x) {
    struct rbt_node *y = x->right;
    x->right = y->left;

    if (y->left)
        y->left->parent = x;

    y->parent = x->parent;
    if (!x->parent)
        tree->root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;

    y->left = x;
    x->parent = y;
}

static void right_rotate(struct rbt *tree, struct rbt_node *y) {
    struct rbt_node *x = y->left;
    y->left = x->right;
    if (x->right)
        x->right->parent = y;

    x->parent = y->parent;
    if (!y->parent)
        tree->root = x;
    else if (y == y->parent->right)
        y->parent->right = x;
    else
        y->parent->left = x;

    x->right = y;
    y->parent = x;
}

static bool rbt_is_black(const struct rbt_node *node) {
    return !node || node->color == TREE_NODE_BLACK;
}

static void fix_deletion(struct rbt *tree, struct rbt_node *x,
                         struct rbt_node *x_parent) {
    size_t steps = 0;
    while (x != tree->root && (!x || x->color == TREE_NODE_BLACK)) {
        if (!x_parent)
            break;

        rbt_debug_step(&steps, x_parent, __func__);

        struct rbt_node *sibling;

        if (x == x_parent->left) {
            sibling = x_parent->right;

            if (sibling && sibling->color == TREE_NODE_RED) {
                sibling->color = TREE_NODE_BLACK;
                x_parent->color = TREE_NODE_RED;
                left_rotate(tree, x_parent);
                sibling = x_parent->right;
            }

            if (!sibling ||
                (rbt_is_black(sibling->left) && rbt_is_black(sibling->right))) {
                if (sibling)
                    sibling->color = TREE_NODE_RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (rbt_is_black(sibling->right)) {
                    if (sibling->left)
                        sibling->left->color = TREE_NODE_BLACK;
                    sibling->color = TREE_NODE_RED;
                    right_rotate(tree, sibling);
                    sibling = x_parent->right;
                }

                sibling->color = x_parent->color;
                x_parent->color = TREE_NODE_BLACK;
                if (sibling->right)
                    sibling->right->color = TREE_NODE_BLACK;
                left_rotate(tree, x_parent);
                x = tree->root;
                x_parent = NULL;
            }
        } else {
            sibling = x_parent->left;

            if (sibling && sibling->color == TREE_NODE_RED) {
                sibling->color = TREE_NODE_BLACK;
                x_parent->color = TREE_NODE_RED;
                right_rotate(tree, x_parent);
                sibling = x_parent->left;
            }

            if (!sibling ||
                (rbt_is_black(sibling->left) && rbt_is_black(sibling->right))) {
                if (sibling)
                    sibling->color = TREE_NODE_RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (rbt_is_black(sibling->left)) {
                    if (sibling->right)
                        sibling->right->color = TREE_NODE_BLACK;
                    sibling->color = TREE_NODE_RED;
                    left_rotate(tree, sibling);
                    sibling = x_parent->left;
                }

                sibling->color = x_parent->color;
                x_parent->color = TREE_NODE_BLACK;
                if (sibling->left)
                    sibling->left->color = TREE_NODE_BLACK;
                right_rotate(tree, x_parent);
                x = tree->root;
                x_parent = NULL;
            }
        }
    }

    if (x)
        x->color = TREE_NODE_BLACK;
}

void rbt_delete(struct rbt *tree, struct rbt_node *z) {
    enforce(z->parent != NULL || z == tree->root,
            "rbt_delete: node %p is not linked into tree %p (root %p)", z, tree,
            tree->root);
    rbt_debug_check_linked(tree, z, __func__);
    rbt_debug_validate(tree, __func__);

    struct rbt_node *y = z;
    struct rbt_node *x = NULL;
    struct rbt_node *x_parent = NULL;
    enum rbt_node_color y_original_color = y->color;

    if (z->left == NULL) {
        x = z->right;
        x_parent = z->parent;
        rb_transplant(tree, z, z->right);
    } else if (z->right == NULL) {
        x = z->left;
        x_parent = z->parent;
        rb_transplant(tree, z, z->left);
    } else {
        y = rbt_find_min(z->right);
        y_original_color = y->color;
        x = y->right;

        if (y->parent != z) {
            x_parent = y->parent;
            rb_transplant(tree, y, y->right);
            y->right = z->right;
            if (y->right)
                y->right->parent = y;
        } else {
            x_parent = y;
        }

        rb_transplant(tree, z, y);
        y->left = z->left;
        if (y->left)
            y->left->parent = y;
        y->color = z->color;
    }

    if (y_original_color == TREE_NODE_BLACK) {
        fix_deletion(tree, x, x_parent);
    }

    z->left = NULL;
    z->right = NULL;
    z->parent = NULL;

    rbt_debug_validate(tree, __func__);
}

static struct rbt_node *
rbt_search_internal(struct rbt *tree, struct rbt_node *root, uint64_t data) {
    while (root && tree->get_data(root) != data) {
        if (data < tree->get_data(root))
            root = root->left;
        else
            root = root->right;
    }
    return root;
}

struct rbt_node *rbt_search(struct rbt *tree, uint64_t data) {
    struct rbt_node *root = tree->root;

    while (root && tree->get_data(root) != data) {
        if (data < tree->get_data(root))
            root = root->left;
        else
            root = root->right;
    }

    return (root && tree->get_data(root) == data) ? root : NULL;
}

bool rbt_has_node(struct rbt *tree, struct rbt_node *node) {
    struct rbt_node *iter;
    rbt_for_each(iter, tree) {
        if (iter == node)
            return true;
    }
    return false;
}

bool rbt_position_valid(struct rbt *tree, struct rbt_node *node) {
    struct rbt_node *prev = rbt_prev(node);
    if (prev && tree->compare(prev, node) > 0)
        return false;

    struct rbt_node *next = rbt_next(node);
    if (next && tree->compare(node, next) > 0)
        return false;

    return true;
}

bool rbt_reinsert(struct rbt *tree, struct rbt_node *node) {
    if (rbt_position_valid(tree, node))
        return false;

    rbt_delete(tree, node);
    rbt_insert(tree, node);
    return true;
}

void rbt_move(struct rbt *from, struct rbt *to, struct rbt_node *node) {
    rbt_delete(from, node);
    rbt_insert(to, node);
}

void rbt_remove(struct rbt *tree, uint64_t data) {
    struct rbt_node *node = rbt_search_internal(tree, tree->root, data);
    if (node)
        rbt_delete(tree, node);
}

static void fix_insertion(struct rbt *tree, struct rbt_node *node) {
    size_t steps = 0;
    while (node != tree->root && node->parent->color == TREE_NODE_RED) {
        rbt_debug_step(&steps, node, __func__);
        struct rbt_node *parent = node->parent;
        struct rbt_node *grandparent = parent->parent;

        if (parent == grandparent->left) {
            struct rbt_node *uncle = grandparent->right;
            if (uncle && uncle->color == TREE_NODE_RED) {
                parent->color = TREE_NODE_BLACK;
                uncle->color = TREE_NODE_BLACK;
                grandparent->color = TREE_NODE_RED;
                node = grandparent;
            } else {
                if (node == parent->right) {
                    node = parent;
                    left_rotate(tree, node);
                    parent = node->parent;
                }
                parent->color = TREE_NODE_BLACK;
                grandparent->color = TREE_NODE_RED;
                right_rotate(tree, grandparent);
            }
        } else {
            struct rbt_node *uncle = grandparent->left;
            if (uncle && uncle->color == TREE_NODE_RED) {
                parent->color = TREE_NODE_BLACK;
                uncle->color = TREE_NODE_BLACK;
                grandparent->color = TREE_NODE_RED;
                node = grandparent;
            } else {
                if (node == parent->left) {
                    node = parent;
                    right_rotate(tree, node);
                    parent = node->parent;
                }
                parent->color = TREE_NODE_BLACK;
                grandparent->color = TREE_NODE_RED;
                left_rotate(tree, grandparent);
            }
        }
    }
    tree->root->color = TREE_NODE_BLACK;
}

void rbt_insert(struct rbt *tree, struct rbt_node *new_node) {
    rbt_debug_validate(tree, __func__);

    new_node->left = NULL;
    new_node->right = NULL;
    new_node->color = TREE_NODE_RED;

    if (tree->root == NULL) {
        new_node->color = TREE_NODE_BLACK;
        new_node->parent = NULL;
        tree->root = new_node;
        return;
    }

    struct rbt_node *current = tree->root;
    struct rbt_node *parent = NULL;
    int32_t cmp = 0;
    size_t steps = 0;
    while (current != NULL) {
#ifdef DEBUG_RBT
        /* Only finds a duplicate whose key still places it on this path */
        if (cc_unlikely(current == new_node))
            panic("rbt_insert: node %p is already in tree %p", new_node, tree);
#endif

        rbt_debug_step(&steps, current, __func__);
        parent = current;
        cmp = tree->compare(new_node, current);
        if (cmp < 0)
            current = current->left;
        else
            current = current->right;
    }

    new_node->parent = parent;

#ifdef DEBUG_LOCK_CHK
    if ((cmp < 0 && parent->left) || (cmp >= 0 && parent->right))
        panic("rbt_insert: node %p would be attached a child of %p "
              "in tree %p, potential subtree orophaning",
              new_node, parent, tree);
#endif

    if (cmp < 0)
        parent->left = new_node;
    else
        parent->right = new_node;

    fix_insertion(tree, new_node);

    if (parent)
        enforce(!(parent->color == TREE_NODE_RED &&
                  new_node->color == TREE_NODE_RED));

    rbt_debug_validate(tree, __func__);
}

void rbt_link_node(struct rbt_node *node, struct rbt_node *parent,
                   struct rbt_node **link) {
    node->parent = parent;
    node->left = NULL;
    node->right = NULL;
    node->color = TREE_NODE_RED;
    *link = node;
}

void rbt_insert_color(struct rbt *tree, struct rbt_node *node) {
    fix_insertion(tree, node);
    rbt_debug_validate(tree, __func__);
}

struct rbt *rbt_init(struct rbt *tree, rbt_get_data get_data, rbt_compare cmp) {
    tree->root = NULL;
    tree->compare = cmp;
    tree->get_data = get_data;
    return tree;
}
