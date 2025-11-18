#include <kassert.h>
#include <structures/pairing_heap.h>

void pairing_heap_init(struct pairing_heap *h, pairing_cmp_t cmp) {
    h->root = NULL;
    h->cmp = cmp;
    spinlock_init(&h->lock);
}

static struct pairing_node *pairing_merge(struct pairing_heap *h,
                                          struct pairing_node *a,
                                          struct pairing_node *b) {
    if (!a)
        return b;

    if (!b)
        return a;

    kassert(spinlock_held(&h->lock));

    if (h->cmp(a, b) <= 0) {
        /* a is smaller, b becomes child of a */
        b->parent = a;
        b->sibling = a->child;
        a->child = b;
        return a;
    } else {
        /* b is smaller, a becomes child of b */
        a->parent = b;
        a->sibling = b->child;
        b->child = a;
        return b;
    }
}

void pairing_heap_insert(struct pairing_heap *h, struct pairing_node *node) {
    enum irql irql = pairing_heap_lock(h);
    node->parent = NULL;
    node->child = NULL;
    node->sibling = NULL;

    h->root = pairing_merge(h, h->root, node);
    pairing_heap_unlock(h, irql);
}

struct pairing_node *pairing_heap_peek(struct pairing_heap *h) {
    enum irql irql = pairing_heap_lock(h);
    struct pairing_node *n = h->root;
    pairing_heap_unlock(h, irql);
    return n;
}

static struct pairing_node *pairing_two_pass(struct pairing_heap *h,
                                             struct pairing_node *first) {
    if (!first || !first->sibling)
        return first;

    kassert(spinlock_held(&h->lock));

    struct pairing_node *a = first;
    struct pairing_node *b = first->sibling;
    struct pairing_node *rest = b->sibling;

    a->sibling = NULL;
    b->sibling = NULL;

    return pairing_merge(h, pairing_merge(h, a, b), pairing_two_pass(h, rest));
}

struct pairing_node *pairing_heap_pop(struct pairing_heap *h) {
    enum irql irql = pairing_heap_lock(h);
    struct pairing_node *root = h->root;

    if (!root)
        goto out;

    h->root = pairing_two_pass(h, root->child);

    if (h->root)
        h->root->parent = NULL;

    root->child = NULL;
    root->sibling = NULL;
    root->parent = NULL;

out:
    pairing_heap_unlock(h, irql);
    return root;
}

void pairing_heap_decrease(struct pairing_heap *h, struct pairing_node *node) {
    /* If already root, nothing to do */
    enum irql irql = pairing_heap_lock(h);
    if (node == h->root)
        goto out;

    /* Cut node from its position */
    struct pairing_node *parent = node->parent;

    /* unlink node from siblings */
    if (parent->child == node) {
        parent->child = node->sibling;
    } else {
        struct pairing_node *s = parent->child;
        while (s->sibling != node)
            s = s->sibling;
        s->sibling = node->sibling;
    }

    node->parent = NULL;
    node->sibling = NULL;

    /* Merge back with root */
    h->root = pairing_merge(h, h->root, node);
out:
    pairing_heap_unlock(h, irql);
}
