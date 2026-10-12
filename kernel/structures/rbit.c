#include <console/panic.h>
#include <enforce.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/rbit.h>

static void rbit_check_cycle(struct rbit_node *node, const char *caller) {
    struct rbit_node *tortoise = node;
    struct rbit_node *hare = node;

    while (hare && hare->left != NULL) {
        tortoise = tortoise ? tortoise->left : NULL;
        hare = hare->left->left;

        if (tortoise == NULL || hare == NULL)
            break;

        if (tortoise == hare) {
            panic("Cycle detected in %s (left spine): tortoise == hare at "
                  "node=%p",
                  caller, (void *) tortoise);
            break;
        }
    }

    tortoise = node;
    hare = node;

    while (hare && hare->right != NULL) {
        tortoise = tortoise ? tortoise->right : NULL;
        hare = hare->right->right;

        if (tortoise == NULL || hare == NULL)
            break;

        if (tortoise == hare) {
            panic("Cycle detected in %s (right spine): tortoise == hare at "
                  "node=%p",
                  caller, (void *) tortoise);
            break;
        }
    }
}

static size_t node_max(struct rbit_node *n) {
    return n ? n->max : 0;
}

static bool default_augment(struct rbit_node *n) {
    size_t old = n->max;
    n->max = MAX(n->interval.high, node_max(n->left), node_max(n->right));
    return n->max != old;
}

static bool node_augment(struct rbit *tree, struct rbit_node *n) {
    if (!n)
        return false;
    return tree->augment ? tree->augment(n) : default_augment(n);
}

static void augment_up(struct rbit *tree, struct rbit_node *n) {
    while (n) {
        node_augment(tree, n);
        n = n->parent;
    }
}

struct rbit *rbit_init(struct rbit *rbit) {
    rbit->root = NULL;
    rbit->augment = NULL;
    return rbit;
}

struct rbit *rbit_tree_create(void) {
    struct rbit *tree = vnew(tree);
    if (!tree)
        return NULL;

    tree->root = NULL;
    tree->augment = NULL;
    return tree;
}

struct rbit_node *rbit_find_min(struct rbit_node *node) {
    rbit_check_cycle(node, __func__);
    while (node && node->left != NULL) {
        node = node->left;
    }
    return node;
}

struct rbit_node *rbit_find_max(struct rbit_node *node) {
    rbit_check_cycle(node, __func__);
    while (node && node->right != NULL) {
        node = node->right;
    }
    return node;
}

struct rbit_node *rbit_min(struct rbit *tree) {
    rbit_check_cycle(tree->root, __func__);
    return rbit_find_min(tree->root);
}

struct rbit_node *rbit_max(struct rbit *tree) {
    rbit_check_cycle(tree->root, __func__);
    return rbit_find_max(tree->root);
}

struct rbit_node *rbit_next(struct rbit_node *node) {
    rbit_check_cycle(node, __func__);

    if (!node)
        return NULL;

    if (node->right)
        return rbit_find_min(node->right);

    struct rbit_node *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}

struct rbit_node *rbit_find_predecessor(struct rbit *tree, size_t low) {
    rbit_check_cycle(tree->root, __func__);
    struct rbit_node *curr = tree->root;
    struct rbit_node *pred = NULL;

    while (curr) {
        if (curr->interval.low < low) {
            pred = curr;
            curr = curr->right;
        } else {
            curr = curr->left;
        }
    }
    return pred;
}

struct rbit_node *rbit_find_successor(struct rbit *tree, size_t low) {
    rbit_check_cycle(tree->root, __func__);
    struct rbit_node *curr = tree->root;
    struct rbit_node *succ = NULL;

    while (curr) {
        if (curr->interval.low > low) {
            succ = curr;
            curr = curr->left;
        } else {
            curr = curr->right;
        }
    }
    return succ;
}

bool rbit_has_node(struct rbit *tree, struct rbit_node *node) {
    rbit_check_cycle(tree->root, __func__);
    struct rbit_node *iter;
    rbit_for_each(iter, tree) {
        if (iter == node)
            return true;
    }
    return false;
}

static void rbit_transplant(struct rbit *tree, struct rbit_node *u,
                            struct rbit_node *v) {
    if (u->parent == NULL)
        tree->root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;

    if (v)
        v->parent = u->parent;
}

static void left_rotate(struct rbit *tree, struct rbit_node *x) {
    struct rbit_node *y = x->right;
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

    /* bottom-up: x (now the lower node) before y (the new apex) */
    node_augment(tree, x);
    node_augment(tree, y);
}

static void right_rotate(struct rbit *tree, struct rbit_node *y) {
    struct rbit_node *x = y->left;
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

    /* bottom-up: y (now the lower node) before x (the new apex) */
    node_augment(tree, y);
    node_augment(tree, x);
}

static bool is_black(struct rbit_node *n) {
    return !n || n->color == RBIT_BLACK;
}

static void fix_deletion(struct rbit *tree, struct rbit_node *x,
                         struct rbit_node *x_parent) {
    while (x != tree->root && is_black(x)) {
        if (!x_parent)
            break;

        struct rbit_node *sibling;

        if (x == x_parent->left) {
            sibling = x_parent->right;

            if (sibling && sibling->color == RBIT_RED) {
                sibling->color = RBIT_BLACK;
                x_parent->color = RBIT_RED;
                left_rotate(tree, x_parent);
                sibling = x_parent->right;
            }

            if (!sibling ||
                (is_black(sibling->left) && is_black(sibling->right))) {
                if (sibling)
                    sibling->color = RBIT_RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (is_black(sibling->right)) {
                    if (sibling->left)
                        sibling->left->color = RBIT_BLACK;
                    sibling->color = RBIT_RED;
                    right_rotate(tree, sibling);
                    sibling = x_parent->right;
                }

                sibling->color = x_parent->color;
                x_parent->color = RBIT_BLACK;
                if (sibling->right)
                    sibling->right->color = RBIT_BLACK;
                left_rotate(tree, x_parent);
                x = tree->root;
                x_parent = NULL;
            }
        } else {
            sibling = x_parent->left;

            if (sibling && sibling->color == RBIT_RED) {
                sibling->color = RBIT_BLACK;
                x_parent->color = RBIT_RED;
                right_rotate(tree, x_parent);
                sibling = x_parent->left;
            }

            if (!sibling ||
                (is_black(sibling->left) && is_black(sibling->right))) {
                if (sibling)
                    sibling->color = RBIT_RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (is_black(sibling->left)) {
                    if (sibling->right)
                        sibling->right->color = RBIT_BLACK;
                    sibling->color = RBIT_RED;
                    left_rotate(tree, sibling);
                    sibling = x_parent->left;
                }

                sibling->color = x_parent->color;
                x_parent->color = RBIT_BLACK;
                if (sibling->left)
                    sibling->left->color = RBIT_BLACK;
                right_rotate(tree, x_parent);
                x = tree->root;
                x_parent = NULL;
            }
        }
    }

    if (x)
        x->color = RBIT_BLACK;
}

static size_t validate_rbit(struct rbit_node *node, size_t *black_height) {
    if (node == NULL) {
        *black_height = 1;
        return 1;
    }

    if (node->color == RBIT_RED) {
        if ((node->left && node->left->color == RBIT_RED) ||
            (node->right && node->right->color == RBIT_RED)) {
            panic("Red-Red violation at node [%zu,%zu]", node->interval.low,
                  node->interval.high);
            return 0;
        }
    }

    size_t left_black_height = 0;
    size_t right_black_height = 0;

    if (!validate_rbit(node->left, &left_black_height))
        return 0;
    if (!validate_rbit(node->right, &right_black_height))
        return 0;

    if (left_black_height != right_black_height) {
        panic("Black-height violation at node [%zu,%zu] (left=%zu, right=%zu)",
              node->interval.low, node->interval.high, left_black_height,
              right_black_height);
        return 0;
    }

    size_t expected_max =
        MAX(node->interval.high, node_max(node->left), node_max(node->right));
    if (node->max != expected_max) {
        panic("Max-invariant violation at node [%zu,%zu]: stored=%zu, "
              "expected=%zu",
              node->interval.low, node->interval.high, node->max, expected_max);
        return 0;
    }

    *black_height = left_black_height + (node->color == RBIT_BLACK ? 1 : 0);
    return 1;
}

void rbit_delete(struct rbit *tree, struct rbit_node *z) {
    enforce(z != NULL);
    enforce(rbit_has_node(tree, z));
    rbit_check_cycle(tree->root, __func__);
    struct rbit_node *y = z;
    struct rbit_node *x = NULL;
    struct rbit_node *x_parent = NULL;
    struct rbit_node *fix_from = NULL;
    enum rbit_color y_original_color = y->color;

    if (z->left == NULL) {
        x = z->right;
        x_parent = z->parent;
        fix_from = z->parent;
        rbit_transplant(tree, z, z->right);
    } else if (z->right == NULL) {
        x = z->left;
        x_parent = z->parent;
        fix_from = z->parent;
        rbit_transplant(tree, z, z->left);
    } else {
        y = rbit_find_min(z->right);
        y_original_color = y->color;
        x = y->right;

        if (y->parent != z) {
            x_parent = y->parent;
            fix_from = y->parent;
            rbit_transplant(tree, y, y->right);
            y->right = z->right;
            if (y->right)
                y->right->parent = y;
        } else {
            x_parent = y;
            fix_from = y;
        }

        rbit_transplant(tree, z, y);
        y->left = z->left;
        if (y->left)
            y->left->parent = y;
        y->color = z->color;
        node_augment(tree, y);
    }

    augment_up(tree, fix_from);

    if (y_original_color == RBIT_BLACK) {
        fix_deletion(tree, x, x_parent);
    }

    if (x)
        augment_up(tree, x);
    augment_up(tree, x_parent);

    z->left = NULL;
    z->right = NULL;
    z->parent = NULL;

    size_t dummy_bh = 0;
    enforce(validate_rbit(tree->root, &dummy_bh));
}

struct rbit_node *rbit_search(struct rbit_node *root, struct interval iv) {
    rbit_check_cycle(root, __func__);
    while (root &&
           !(root->interval.low == iv.low && root->interval.high == iv.high)) {
        if (iv.low < root->interval.low)
            root = root->left;
        else
            root = root->right;
    }
    return root;
}

static int intervals_overlap(struct interval a, struct interval b) {
    return a.low <= b.high && b.low <= a.high;
}

struct rbit_node *rbit_overlap_search(struct rbit_node *root,
                                      struct interval iv) {
    rbit_check_cycle(root, __func__);
    while (root && !intervals_overlap(root->interval, iv)) {
        if (root->left && root->left->max >= iv.low)
            root = root->left;
        else
            root = root->right;
    }
    return root;
}

void rbit_remove(struct rbit *tree, struct interval iv) {
    struct rbit_node *node = rbit_search(tree->root, iv);
    if (node)
        rbit_delete(tree, node);
}

static void fix_insertion(struct rbit *tree, struct rbit_node *node) {
    while (node != tree->root && node->parent->color == RBIT_RED) {
        struct rbit_node *parent = node->parent;
        struct rbit_node *grandparent = parent->parent;

        if (parent == grandparent->left) {
            struct rbit_node *uncle = grandparent->right;
            if (uncle && uncle->color == RBIT_RED) {
                parent->color = RBIT_BLACK;
                uncle->color = RBIT_BLACK;
                grandparent->color = RBIT_RED;
                node = grandparent;
            } else {
                if (node == parent->right) {
                    node = parent;
                    left_rotate(tree, node);
                    parent = node->parent;
                }
                parent->color = RBIT_BLACK;
                grandparent->color = RBIT_RED;
                right_rotate(tree, grandparent);
            }
        } else {
            struct rbit_node *uncle = grandparent->left;
            if (uncle && uncle->color == RBIT_RED) {
                parent->color = RBIT_BLACK;
                uncle->color = RBIT_BLACK;
                grandparent->color = RBIT_RED;
                node = grandparent;
            } else {
                if (node == parent->left) {
                    node = parent;
                    right_rotate(tree, node);
                    parent = node->parent;
                }
                parent->color = RBIT_BLACK;
                grandparent->color = RBIT_RED;
                left_rotate(tree, grandparent);
            }
        }
    }
    tree->root->color = RBIT_BLACK;
}

void rbit_insert(struct rbit *tree, struct rbit_node *new_node) {
    enforce(new_node != NULL);
    enforce(!rbit_has_node(tree, new_node));
    rbit_check_cycle(tree->root, __func__);
    new_node->left = NULL;
    new_node->right = NULL;
    new_node->color = RBIT_RED;
    node_augment(tree, new_node);

    if (tree->root == NULL) {
        new_node->color = RBIT_BLACK;
        new_node->parent = NULL;
        tree->root = new_node;
        return;
    }

    struct rbit_node *current = tree->root;
    struct rbit_node *parent = NULL;
    while (current != NULL) {
        parent = current;
        if (new_node->interval.low < current->interval.low)
            current = current->left;
        else
            current = current->right;
    }

    new_node->parent = parent;
    if (new_node->interval.low < parent->interval.low)
        parent->left = new_node;
    else
        parent->right = new_node;

    augment_up(tree, parent);

    fix_insertion(tree, new_node);

    if (parent)
        enforce(!(parent->color == RBIT_RED && new_node->color == RBIT_RED));

    size_t dummy_bh = 0;
    enforce(validate_rbit(tree->root, &dummy_bh));
}
