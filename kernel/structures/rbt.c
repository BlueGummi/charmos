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

    while (rbt_parent(n)) {
        const struct rbt_node *p = rbt_parent(n);
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

    if (cc_unlikely(node->left && rbt_parent(node->left) != node))
        panic("%s: node %p left child %p has parent %p, tree %p", caller, node,
              node->left, rbt_parent(node->left), tree);

    if (cc_unlikely(node->right && rbt_parent(node->right) != node))
        panic("%s: node %p right child %p has parent %p, tree %p", caller, node,
              node->right, rbt_parent(node->right), tree);

    if (cc_unlikely(rbt_color(node) == TREE_NODE_RED &&
                    ((node->left && rbt_color(node->left) == TREE_NODE_RED) ||
                     (node->right && rbt_color(node->right) == TREE_NODE_RED))))
        panic("%s: red-red violation at node %p, tree %p", caller, node, tree);

    size_t lh = rbt_debug_validate_subtree(tree, node->left, depth + 1, caller);
    size_t rh =
        rbt_debug_validate_subtree(tree, node->right, depth + 1, caller);

    if (cc_unlikely(lh != rh))
        panic("%s: black-height violation at node %p (left %zu, right %zu), "
              "tree %p",
              caller, node, lh, rh, tree);

    return lh + (rbt_color(node) == TREE_NODE_BLACK ? 1 : 0);
}

static void rbt_debug_validate(const struct rbt *tree, const char *caller) {
    const struct rbt_node *root = tree->root;
    if (!root)
        return;

    if (cc_unlikely(rbt_parent(root)))
        panic("%s: root %p of tree %p has parent %p", caller, root, tree,
              rbt_parent(root));

    if (cc_unlikely(rbt_color(root) != TREE_NODE_BLACK))
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
    struct rbt_node *parent = rbt_parent(node);
    while (parent && node == parent->right) {
        node = parent;
        parent = rbt_parent(parent);
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
    if (rbt_parent(u) == NULL)
        tree->root = v;
    else if (u == rbt_parent(u)->left)
        rbt_parent(u)->left = v;
    else
        rbt_parent(u)->right = v;

    if (v)
        rbt_set_parent(v, rbt_parent(u));
}

static void left_rotate(struct rbt *tree, struct rbt_node *x) {
    struct rbt_node *y = x->right;
    x->right = y->left;

    if (y->left)
        rbt_set_parent(y->left, x);

    rbt_set_parent(y, rbt_parent(x));
    if (!rbt_parent(x))
        tree->root = y;
    else if (x == rbt_parent(x)->left)
        rbt_parent(x)->left = y;
    else
        rbt_parent(x)->right = y;

    y->left = x;
    rbt_set_parent(x, y);
}

static void right_rotate(struct rbt *tree, struct rbt_node *y) {
    struct rbt_node *x = y->left;
    y->left = x->right;
    if (x->right)
        rbt_set_parent(x->right, y);

    rbt_set_parent(x, rbt_parent(y));
    if (!rbt_parent(y))
        tree->root = x;
    else if (y == rbt_parent(y)->right)
        rbt_parent(y)->right = x;
    else
        rbt_parent(y)->left = x;

    x->right = y;
    rbt_set_parent(y, x);
}

static bool rbt_is_black(const struct rbt_node *node) {
    return !node || rbt_color(node) == TREE_NODE_BLACK;
}

static void fix_deletion(struct rbt *tree, struct rbt_node *x,
                         struct rbt_node *x_parent) {
    size_t steps = 0;
    while (x != tree->root && (!x || rbt_color(x) == TREE_NODE_BLACK)) {
        if (!x_parent)
            break;

        rbt_debug_step(&steps, x_parent, __func__);

        struct rbt_node *sibling;

        if (x == x_parent->left) {
            sibling = x_parent->right;

            if (sibling && rbt_color(sibling) == TREE_NODE_RED) {
                rbt_set_color(sibling, TREE_NODE_BLACK);
                rbt_set_color(x_parent, TREE_NODE_RED);
                left_rotate(tree, x_parent);
                sibling = x_parent->right;
            }

            if (!sibling ||
                (rbt_is_black(sibling->left) && rbt_is_black(sibling->right))) {
                if (sibling)
                    rbt_set_color(sibling, TREE_NODE_RED);
                x = x_parent;
                x_parent = rbt_parent(x);
            } else {
                if (rbt_is_black(sibling->right)) {
                    if (sibling->left)
                        rbt_set_color(sibling->left, TREE_NODE_BLACK);
                    rbt_set_color(sibling, TREE_NODE_RED);
                    right_rotate(tree, sibling);
                    sibling = x_parent->right;
                }

                rbt_set_color(sibling, rbt_color(x_parent));
                rbt_set_color(x_parent, TREE_NODE_BLACK);
                if (sibling->right)
                    rbt_set_color(sibling->right, TREE_NODE_BLACK);
                left_rotate(tree, x_parent);
                x = tree->root;
                x_parent = NULL;
            }
        } else {
            sibling = x_parent->left;

            if (sibling && rbt_color(sibling) == TREE_NODE_RED) {
                rbt_set_color(sibling, TREE_NODE_BLACK);
                rbt_set_color(x_parent, TREE_NODE_RED);
                right_rotate(tree, x_parent);
                sibling = x_parent->left;
            }

            if (!sibling ||
                (rbt_is_black(sibling->left) && rbt_is_black(sibling->right))) {
                if (sibling)
                    rbt_set_color(sibling, TREE_NODE_RED);
                x = x_parent;
                x_parent = rbt_parent(x);
            } else {
                if (rbt_is_black(sibling->left)) {
                    if (sibling->right)
                        rbt_set_color(sibling->right, TREE_NODE_BLACK);
                    rbt_set_color(sibling, TREE_NODE_RED);
                    left_rotate(tree, sibling);
                    sibling = x_parent->left;
                }

                rbt_set_color(sibling, rbt_color(x_parent));
                rbt_set_color(x_parent, TREE_NODE_BLACK);
                if (sibling->left)
                    rbt_set_color(sibling->left, TREE_NODE_BLACK);
                right_rotate(tree, x_parent);
                x = tree->root;
                x_parent = NULL;
            }
        }
    }

    if (x)
        rbt_set_color(x, TREE_NODE_BLACK);
}

void rbt_delete(struct rbt *tree, struct rbt_node *z) {
    enforce(rbt_parent(z) != NULL || z == tree->root,
            "rbt_delete: node %p is not linked into tree %p (root %p)", z, tree,
            tree->root);
    rbt_debug_check_linked(tree, z, __func__);
    rbt_debug_validate(tree, __func__);

    struct rbt_node *y = z;
    struct rbt_node *x = NULL;
    struct rbt_node *x_parent = NULL;
    enum rbt_node_color y_original_color = rbt_color(y);

    if (z->left == NULL) {
        x = z->right;
        x_parent = rbt_parent(z);
        rb_transplant(tree, z, z->right);
    } else if (z->right == NULL) {
        x = z->left;
        x_parent = rbt_parent(z);
        rb_transplant(tree, z, z->left);
    } else {
        y = rbt_find_min(z->right);
        y_original_color = rbt_color(y);
        x = y->right;

        if (rbt_parent(y) != z) {
            x_parent = rbt_parent(y);
            rb_transplant(tree, y, y->right);
            y->right = z->right;
            if (y->right)
                rbt_set_parent(y->right, y);
        } else {
            x_parent = y;
        }

        rb_transplant(tree, z, y);
        y->left = z->left;
        if (y->left)
            rbt_set_parent(y->left, y);
        rbt_set_color(y, rbt_color(z));
    }

    if (y_original_color == TREE_NODE_BLACK) {
        fix_deletion(tree, x, x_parent);
    }

    z->left = NULL;
    z->right = NULL;
    rbt_set_parent(z, NULL);

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
    while (node != tree->root && rbt_color(rbt_parent(node)) == TREE_NODE_RED) {
        rbt_debug_step(&steps, node, __func__);
        struct rbt_node *parent = rbt_parent(node);
        struct rbt_node *grandparent = rbt_parent(parent);

        if (parent == grandparent->left) {
            struct rbt_node *uncle = grandparent->right;
            if (uncle && rbt_color(uncle) == TREE_NODE_RED) {
                rbt_set_color(parent, TREE_NODE_BLACK);
                rbt_set_color(uncle, TREE_NODE_BLACK);
                rbt_set_color(grandparent, TREE_NODE_RED);
                node = grandparent;
            } else {
                if (node == parent->right) {
                    node = parent;
                    left_rotate(tree, node);
                    parent = rbt_parent(node);
                }
                rbt_set_color(parent, TREE_NODE_BLACK);
                rbt_set_color(grandparent, TREE_NODE_RED);
                right_rotate(tree, grandparent);
            }
        } else {
            struct rbt_node *uncle = grandparent->left;
            if (uncle && rbt_color(uncle) == TREE_NODE_RED) {
                rbt_set_color(parent, TREE_NODE_BLACK);
                rbt_set_color(uncle, TREE_NODE_BLACK);
                rbt_set_color(grandparent, TREE_NODE_RED);
                node = grandparent;
            } else {
                if (node == parent->left) {
                    node = parent;
                    right_rotate(tree, node);
                    parent = rbt_parent(node);
                }
                rbt_set_color(parent, TREE_NODE_BLACK);
                rbt_set_color(grandparent, TREE_NODE_RED);
                left_rotate(tree, grandparent);
            }
        }
    }
    rbt_set_color(tree->root, TREE_NODE_BLACK);
}

void rbt_insert(struct rbt *tree, struct rbt_node *new_node) {
    rbt_debug_validate(tree, __func__);

    new_node->left = NULL;
    new_node->right = NULL;
    rbt_set_color(new_node, TREE_NODE_RED);

    if (tree->root == NULL) {
        rbt_set_color(new_node, TREE_NODE_BLACK);
        rbt_set_parent(new_node, NULL);
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

    rbt_set_parent(new_node, parent);

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
        enforce(!(rbt_color(parent) == TREE_NODE_RED &&
                  rbt_color(new_node) == TREE_NODE_RED));

    rbt_debug_validate(tree, __func__);
}

void rbt_link_node(struct rbt_node *node, struct rbt_node *parent,
                   struct rbt_node **link) {
    rbt_set_parent(node, parent);
    node->left = NULL;
    node->right = NULL;
    rbt_set_color(node, TREE_NODE_RED);
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
