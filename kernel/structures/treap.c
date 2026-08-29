#include <structures/treap.h>

static void rotate_left(struct treap_tree *tree, struct treap_node *x) {
    struct treap_node *y = x->right;
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

static void rotate_right(struct treap_tree *tree, struct treap_node *y) {
    struct treap_node *x = y->left;
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

void treap_tree_init(struct treap_tree *tree,
                     const struct treap_node_ops *ops) {
    tree->root = NULL;
    tree->ops = ops;
}

void treap_insert(struct treap_tree *tree, struct treap_node *node) {
    node->left = node->right = node->parent = NULL;

    if (!tree->root) {
        tree->root = node;
        return;
    }

    struct treap_node *cur = tree->root;
    struct treap_node *p = NULL;
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

    while (node->parent && node->priority < node->parent->priority) {
        if (node == node->parent->left) {
            rotate_right(tree, node->parent);
        } else {
            rotate_left(tree, node->parent);
        }
    }
}

void treap_remove(struct treap_tree *tree, struct treap_node *node) {
    if (!tree || !tree->root || !node) {
        return;
    }

    while (node->left || node->right) {
        if (node->left && node->right) {
            if (node->left->priority < node->right->priority) {
                rotate_right(tree, node);
            } else {
                rotate_left(tree, node);
            }
        } else if (node->left) {
            rotate_right(tree, node);
        } else {
            rotate_left(tree, node);
        }
    }

    if (node->parent) {
        if (node == node->parent->left) {
            node->parent->left = NULL;
        } else {
            node->parent->right = NULL;
        }
    } else {
        tree->root = NULL;
    }

    node->left = node->right = node->parent = NULL;
}

struct treap_node *treap_find(const struct treap_tree *tree, const void *key) {
    if (!tree || !tree->root) {
        return NULL;
    }

    struct treap_node *cur = tree->root;
    while (cur) {
        int cmp = tree->ops->cmp_key(cur, key);
        if (cmp == 0) {
            return cur;
        } else if (cmp > 0) {
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }

    return NULL;
}

struct treap_node *treap_first(const struct treap_tree *tree) {
    if (!tree || !tree->root) {
        return NULL;
    }
    struct treap_node *node = tree->root;
    while (node->left) {
        node = node->left;
    }
    return node;
}

struct treap_node *treap_last(const struct treap_tree *tree) {
    if (!tree || !tree->root) {
        return NULL;
    }
    struct treap_node *node = tree->root;
    while (node->right) {
        node = node->right;
    }
    return node;
}

struct treap_node *treap_next(const struct treap_node *node) {
    if (!node) {
        return NULL;
    }

    if (node->right) {
        node = node->right;
        while (node->left) {
            node = node->left;
        }
        return (struct treap_node *) node;
    }

    struct treap_node *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}

struct treap_node *treap_prev(const struct treap_node *node) {
    if (!node) {
        return NULL;
    }

    if (node->left) {
        node = node->left;
        while (node->right) {
            node = node->right;
        }
        return (struct treap_node *) node;
    }

    struct treap_node *parent = node->parent;
    while (parent && node == parent->left) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}
