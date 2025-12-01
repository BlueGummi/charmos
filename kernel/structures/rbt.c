#include <console/printf.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/rbt.h>

struct rbt *rbt_create(rbt_get_data get) {
    struct rbt *tree = kmalloc(sizeof(struct rbt), ALLOC_PARAMS_DEFAULT);
    if (!tree)
        return NULL;

    tree->get_data = get;
    tree->root = NULL;
    return tree;
}

struct rbt_node *rbt_find_min(struct rbt_node *node) {
    while (node && node->left != NULL)
        node = node->left;

    return node;
}

struct rbt_node *rbt_find_max(struct rbt_node *node) {
    while (node && node->right != NULL)
        node = node->right;

    return node;
}

struct rbt_node *rbt_max(struct rbt *tree) {
    return rbt_find_max(tree->root);
}

struct rbt_node *rbt_min(struct rbt *tree) {
    return rbt_find_min(tree->root);
}

struct rbt_node *rbt_next(struct rbt_node *node) {
    if (node->right)
        return rbt_find_min(node->right);

    struct rbt_node *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
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

static void fix_deletion(struct rbt *tree, struct rbt_node *x) {
    while (x != tree->root && (!x || x->color == TREE_NODE_BLACK)) {
        if (!x || !x->parent)
            break;

        struct rbt_node *sibling;

        if (x == x->parent->left) {
            sibling = x->parent->right;

            if (sibling && sibling->color == TREE_NODE_RED) {
                sibling->color = TREE_NODE_BLACK;
                x->parent->color = TREE_NODE_RED;
                left_rotate(tree, x->parent);
                sibling = x->parent->right;
            }

            if (!sibling ||
                ((!sibling->left || sibling->left->color == TREE_NODE_BLACK) &&
                 (!sibling->right ||
                  sibling->right->color == TREE_NODE_BLACK))) {
                if (sibling)
                    sibling->color = TREE_NODE_RED;
                x = x->parent;
            } else {
                if (!sibling->right ||
                    sibling->right->color == TREE_NODE_BLACK) {
                    if (sibling->left)
                        sibling->left->color = TREE_NODE_BLACK;
                    if (sibling) {
                        sibling->color = TREE_NODE_RED;
                        right_rotate(tree, sibling);
                        sibling = x->parent->right;
                    }
                }

                if (sibling) {
                    sibling->color = x->parent->color;
                    x->parent->color = TREE_NODE_BLACK;
                    if (sibling->right)
                        sibling->right->color = TREE_NODE_BLACK;
                    left_rotate(tree, x->parent);
                }
                x = tree->root;
            }
        } else {
            sibling = x->parent->left;

            if (sibling && sibling->color == TREE_NODE_RED) {
                sibling->color = TREE_NODE_BLACK;
                x->parent->color = TREE_NODE_RED;
                right_rotate(tree, x->parent);
                sibling = x->parent->left;
            }

            if (!sibling ||
                ((!sibling->left || sibling->left->color == TREE_NODE_BLACK) &&
                 (!sibling->right ||
                  sibling->right->color == TREE_NODE_BLACK))) {
                if (sibling)
                    sibling->color = TREE_NODE_RED;
                x = x->parent;
            } else {
                if (!sibling->left || sibling->left->color == TREE_NODE_BLACK) {
                    if (sibling->right)
                        sibling->right->color = TREE_NODE_BLACK;
                    if (sibling) {
                        sibling->color = TREE_NODE_RED;
                        left_rotate(tree, sibling);
                        sibling = x->parent->left;
                    }
                }

                if (sibling) {
                    sibling->color = x->parent->color;
                    x->parent->color = TREE_NODE_BLACK;
                    if (sibling->left)
                        sibling->left->color = TREE_NODE_BLACK;
                    right_rotate(tree, x->parent);
                }
                x = tree->root;
            }
        }
    }

    if (x)
        x->color = TREE_NODE_BLACK;
}

void rb_delete(struct rbt *tree, struct rbt_node *z) {
    struct rbt_node *y = z;
    struct rbt_node *x = NULL;
    enum rbt_node_color y_original_color = y->color;

    if (z->left == NULL) {
        x = z->right;
        rb_transplant(tree, z, z->right);
    } else if (z->right == NULL) {
        x = z->left;
        rb_transplant(tree, z, z->left);
    } else {
        y = rbt_find_min(z->right);
        y_original_color = y->color;
        x = y->right;

        if (y->parent != z) {
            rb_transplant(tree, y, y->right);
            y->right = z->right;
            if (y->right)
                y->right->parent = y;
        }

        rb_transplant(tree, z, y);
        y->left = z->left;
        if (y->left)
            y->left->parent = y;
        y->color = z->color;
    }

    if (y_original_color == TREE_NODE_BLACK) {
        fix_deletion(tree, x);
    }

    z->left = NULL;
    z->right = NULL;
    z->parent = NULL;
}

struct rbt_node *rbt_search(struct rbt *tree, struct rbt_node *root,
                            uint64_t data) {
    while (root && tree->get_data(root) != data) {
        if (data < tree->get_data(root))
            root = root->left;
        else
            root = root->right;
    }
    return root;
}

void rbt_remove(struct rbt *tree, uint64_t data) {
    struct rbt_node *node = rbt_search(tree, tree->root, data);
    if (node)
        rb_delete(tree, node);
}

static void fix_insertion(struct rbt *tree, struct rbt_node *node) {
    while (node != tree->root && node->parent->color == TREE_NODE_RED) {
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
    while (current != NULL) {
        parent = current;
        if (tree->get_data(new_node) < tree->get_data(current))
            current = current->left;
        else
            current = current->right;
    }

    new_node->parent = parent;
    if (tree->get_data(new_node) < tree->get_data(parent))
        parent->left = new_node;
    else
        parent->right = new_node;

    fix_insertion(tree, new_node);

    if (parent)
        kassert(!(parent->color == TREE_NODE_RED &&
                  new_node->color == TREE_NODE_RED));
}

struct rbt *rbt_init(struct rbt *tree, rbt_get_data get_data) {
    tree->root = NULL;
    tree->get_data = get_data;
    return tree;
}
