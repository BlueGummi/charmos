/* @title: Pairing heap */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>

struct pairing_node {
    struct pairing_node *parent;
    struct pairing_node *child;
    struct pairing_node *sibling;
};

typedef int32_t (*pairing_cmp_t)(struct pairing_node *, struct pairing_node *);

struct pairing_heap {
    struct pairing_node *root;
    pairing_cmp_t cmp;
    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(pairing_heap, lock);
void pairing_heap_init(struct pairing_heap *h, pairing_cmp_t cmp);
void pairing_heap_insert(struct pairing_heap *h, struct pairing_node *node);
struct pairing_node *pairing_heap_peek(struct pairing_heap *h);
struct pairing_node *pairing_heap_pop(struct pairing_heap *h);
void pairing_heap_decrease(struct pairing_heap *h, struct pairing_node *node);
