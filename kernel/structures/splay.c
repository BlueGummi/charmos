#include <structures/splay.h>

static void rotate_left(struct splay_tree *tree, struct splay_node *x) {
    struct splay_node *y = x->right;
    x->right = y->left;
    if (y->left) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (!x->parent) {
        tree->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

static void rotate_right(struct splay_tree *tree, struct splay_node *y) {
    struct splay_node *x = y->left;
    y->left = x->right;
    if (x->right) {
        x->right->parent = y;
    }
    x->parent = y->parent;
    if (!y->parent) {
        tree->root = x;
    } else if (y == y->parent->left) {
        y->parent->left = x;
    } else {
        y->parent->right = x;
    }
    x->right = y;
    y->parent = x;
}

void splay(struct splay_tree *tree, struct splay_node *x) {
    if (!tree || !x) {
        return;
    }

    while (x->parent) {
        if (!x->parent->parent) {
            /* Zig */
            if (x->parent->left == x) {
                rotate_right(tree, x->parent);
            } else {
                rotate_left(tree, x->parent);
            }
        } else if (x->parent->left == x &&
                   x->parent->parent->left == x->parent) {
            /* Zig-Zig (left-left) */
            rotate_right(tree, x->parent->parent);
            rotate_right(tree, x->parent);
        } else if (x->parent->right == x &&
                   x->parent->parent->right == x->parent) {
            /* Zig-Zig (right-right) */
            rotate_left(tree, x->parent->parent);
            rotate_left(tree, x->parent);
        } else if (x->parent->left == x &&
                   x->parent->parent->right == x->parent) {
            /* Zig-Zag (right-left) */
            rotate_right(tree, x->parent);
            rotate_left(tree, x->parent);
        } else {
            /* Zig-Zag (left-right) */
            rotate_left(tree, x->parent);
            rotate_right(tree, x->parent);
        }
    }
}

void splay_tree_init(struct splay_tree *tree,
                     const struct splay_node_ops *ops) {
    tree->root = NULL;
    tree->ops = ops;
}

void splay_insert(struct splay_tree *tree, struct splay_node *node) {
    splay_init_node(node);

    if (!tree->root) {
        tree->root = node;
        return;
    }

    struct splay_node *cur = tree->root;
    struct splay_node *p = NULL;
    int cmp = 0;

    while (cur) {
        p = cur;
        cmp = tree->ops->cmp(node, cur);
        if (cmp < 0) {
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }

    node->parent = p;
    if (cmp < 0) {
        p->left = node;
    } else {
        p->right = node;
    }

    splay(tree, node);
}

void splay_remove(struct splay_tree *tree, struct splay_node *node) {
    if (!tree || !tree->root || !node) {
        return;
    }

    splay(tree, node);

    if (!node->left) {
        tree->root = node->right;
        if (tree->root) {
            tree->root->parent = NULL;
        }
    } else {
        struct splay_node *left_sub = node->left;
        left_sub->parent = NULL;

        struct splay_node *max = left_sub;
        while (max->right) {
            max = max->right;
        }

        tree->root = left_sub;
        splay(tree, max);

        max->right = node->right;
        if (max->right) {
            max->right->parent = max;
        }
        tree->root = max;
    }

    splay_init_node(node);
}

struct splay_node *splay_find(struct splay_tree *tree, const void *key) {
    if (!tree || !tree->root) {
        return NULL;
    }

    struct splay_node *x = tree->root;
    struct splay_node *last = NULL;

    while (x) {
        last = x;
        int cmp = tree->ops->cmp_key(x, key);
        if (cmp == 0) {
            break;
        } else if (cmp > 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }

    if (last) {
        splay(tree, last);
    }

    if (x && tree->ops->cmp_key(x, key) == 0) {
        return x;
    }

    return NULL;
}

struct splay_node *splay_first(const struct splay_tree *tree) {
    if (!tree || !tree->root) {
        return NULL;
    }
    struct splay_node *node = tree->root;
    while (node->left) {
        node = node->left;
    }
    return node;
}

struct splay_node *splay_last(const struct splay_tree *tree) {
    if (!tree || !tree->root) {
        return NULL;
    }
    struct splay_node *node = tree->root;
    while (node->right) {
        node = node->right;
    }
    return node;
}

struct splay_node *splay_next(const struct splay_node *node) {
    if (!node) {
        return NULL;
    }

    if (node->right) {
        node = node->right;
        while (node->left) {
            node = node->left;
        }
        return (struct splay_node *) node;
    }

    struct splay_node *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}

struct splay_node *splay_prev(const struct splay_node *node) {
    if (!node) {
        return NULL;
    }

    if (node->left) {
        node = node->left;
        while (node->right) {
            node = node->right;
        }
        return (struct splay_node *) node;
    }

    struct splay_node *parent = node->parent;
    while (parent && node == parent->left) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}
