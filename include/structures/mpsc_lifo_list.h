/* @title: Lockless MPSC LIFO list */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>

struct mpsc_lifo_list_node {
    struct mpsc_lifo_list_node *next;
};

struct mpsc_lifo_list {
    atomic(struct mpsc_lifo_list_node *) head;
};

#define MPSC_LIFO_LIST_INIT {NULL}
#define MPSC_LIFO_LIST(name) struct mpsc_lifo_list name = MPSC_LIFO_LIST_INIT

static inline void mpsc_lifo_list_init(struct mpsc_lifo_list *q) {
    atomic_store_rlx(&q->head, NULL);
}

static inline int mpsc_lifo_list_empty(const struct mpsc_lifo_list *q) {
    return atomic_load_acq(&q->head) == NULL;
}

/* Returns true if list was previously empty */
static inline bool mpsc_lifo_list_push(struct mpsc_lifo_list *q,
                                       struct mpsc_lifo_list_node *n) {
    struct mpsc_lifo_list_node *old = atomic_load_rlx(&q->head);
    do {
        n->next = old;
    } while (!atomic_cas_weak(&q->head, &old, n, mo_release, mo_rlx));

    return old == NULL;
}

/* Return the entire chain and mark as empty */
static inline struct mpsc_lifo_list_node *
mpsc_lifo_list_drain(struct mpsc_lifo_list *q) {
    return atomic_xchg_acq(&q->head, NULL);
}

/* Reverse a detached chain to get the FIFO list order */
static inline struct mpsc_lifo_list_node *
mpsc_lifo_list_reverse(struct mpsc_lifo_list_node *chain) {
    struct mpsc_lifo_list_node *prev = NULL;
    while (chain) {
        struct mpsc_lifo_list_node *next = chain->next;
        chain->next = prev;
        prev = chain;
        chain = next;
    }
    return prev;
}

/* Single consumer pop */
static inline struct mpsc_lifo_list_node *
mpsc_lifo_list_pop(struct mpsc_lifo_list *q) {
    struct mpsc_lifo_list_node *old = atomic_load_acq(&q->head);
    while (old) {
        if (atomic_cas_weak(&q->head, &old, old->next, mo_acquire, mo_acquire))
            return old;
    }
    return NULL;
}

#define mpsc_lifo_list_entry(ptr, type, member) container_of(ptr, type, member)

/* Detached chain */
#define mpsc_lifo_list_for_each(pos, chain)                                    \
    for (pos = (chain); pos; pos = pos->next)

#define mpsc_lifo_list_for_each_safe(pos, n, chain)                            \
    for (pos = (chain), n = (pos) ? (pos)->next : NULL; pos;                   \
         pos = n, n = (pos) ? (pos)->next : NULL)

#define mpsc_lifo_list_for_each_entry(pos, chain, member)                      \
    for (pos = (chain) ? mpsc_lifo_list_entry((chain), typeof(*pos), member)   \
                       : NULL;                                                 \
         pos;                                                                  \
         pos = (pos)->member.next ? mpsc_lifo_list_entry((pos)->member.next,   \
                                                         typeof(*pos), member) \
                                  : NULL)
