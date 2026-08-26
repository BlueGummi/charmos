/* @title: Lockless MPSC singly linked list */
#pragma once
#include <compiler.h>
#include <container_of.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

struct mpsc_slist_node {
    struct mpsc_slist_node *next;
};

struct mpsc_slist {
    _Atomic(struct mpsc_slist_node *) head;
};

#define MPSC_SLIST_INIT {NULL}
#define MPSC_SLIST(name) struct mpsc_slist name = MPSC_SLIST_INIT

static inline void mpsc_slist_init(struct mpsc_slist *q) {
    atomic_store_explicit(&q->head, NULL, memory_order_relaxed);
}

static inline int mpsc_slist_empty(const struct mpsc_slist *q) {
    return atomic_load_explicit(&q->head, memory_order_acquire) == NULL;
}

/* Returns true if list was previously empty */
static inline bool mpsc_slist_push(struct mpsc_slist *q,
                                   struct mpsc_slist_node *n) {
    struct mpsc_slist_node *old =
        atomic_load_explicit(&q->head, memory_order_relaxed);
    do {
        n->next = old;
    } while (!atomic_compare_exchange_weak_explicit(
        &q->head, &old, n, memory_order_release, memory_order_relaxed));

    return old == NULL;
}

/* Return the entire chain and mark as empty */
static inline struct mpsc_slist_node *mpsc_slist_drain(struct mpsc_slist *q) {
    return atomic_exchange_explicit(&q->head, NULL, memory_order_acquire);
}

/* Reverse a detached chain to get the FIFO list order */
static inline struct mpsc_slist_node *
mpsc_slist_reverse(struct mpsc_slist_node *chain) {
    struct mpsc_slist_node *prev = NULL;
    while (chain) {
        struct mpsc_slist_node *next = chain->next;
        chain->next = prev;
        prev = chain;
        chain = next;
    }
    return prev;
}

/* Single consumer pop */
static inline struct mpsc_slist_node *mpsc_slist_pop_one(struct mpsc_slist *q) {
    struct mpsc_slist_node *old =
        atomic_load_explicit(&q->head, memory_order_acquire);
    while (old) {
        if (atomic_compare_exchange_weak_explicit(&q->head, &old, old->next,
                                                  memory_order_acquire,
                                                  memory_order_acquire))
            return old;
    }
    return NULL;
}

#define mpsc_slist_entry(ptr, type, member) container_of(ptr, type, member)

/* Detached chain */
#define mpsc_slist_for_each(pos, chain)                                        \
    for (pos = (chain); pos; pos = pos->next)

#define mpsc_slist_for_each_safe(pos, n, chain)                                \
    for (pos = (chain), n = (pos) ? (pos)->next : NULL; pos;                   \
         pos = n, n = (pos) ? (pos)->next : NULL)

#define mpsc_slist_for_each_entry(pos, chain, member)                          \
    for (pos = (chain) ? mpsc_slist_entry((chain), typeof(*pos), member)       \
                       : NULL;                                                 \
         pos;                                                                  \
         pos = (pos)->member.next ? mpsc_slist_entry((pos)->member.next,       \
                                                     typeof(*pos), member)     \
                                  : NULL)
