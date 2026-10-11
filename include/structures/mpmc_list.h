/* @title: Lockless MPMC singly linked list */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

struct mpmc_slist_node {
    struct mpmc_slist_node *next;
};

union mpmc_slist_head {
    struct {
        struct mpmc_slist_node *node;
        uint64_t seq;
    };
    uint128_t raw;
};

struct mpmc_slist {
    atomic_uint128_t head;
};

#define MPMC_SLIST_INIT {.head = 0}
#define MPMC_SLIST(name) struct mpmc_slist name = MPMC_SLIST_INIT

/* NOTE: my syntax highlighter breaks when I actually insert
 * the keyword struct/union into the full versions, not sure why */
ct_assert_union_size_eq(mpmc_slist_head, 16);
ct_assert_struct_size_eq(mpmc_slist, 16);
ct_assert_struct_align_eq(mpmc_slist, 16);

static inline bool mpmc_slist_cas(struct mpmc_slist *q,
                                  union mpmc_slist_head *expected,
                                  union mpmc_slist_head desired) {
    return atomic_cas_strong(&q->head, &expected->raw, desired.raw, mo_acq_rel,
                             mo_acquire);
}

static inline union mpmc_slist_head
mpmc_slist_load(const struct mpmc_slist *q) {
    return (union mpmc_slist_head) {.raw = atomic_load_acq(&q->head)};
}

static inline void mpmc_slist_init(struct mpmc_slist *q) {
    atomic_store_rlx(&q->head, 0);
}

static inline bool mpmc_slist_empty(const struct mpmc_slist *q) {
    union mpmc_slist_head h = mpmc_slist_load(q);
    return h.node == NULL;
}

static inline uint64_t mpmc_slist_seq(const struct mpmc_slist *q) {
    union mpmc_slist_head h = mpmc_slist_load(q);
    return h.seq;
}

static inline struct mpmc_slist_node *
mpmc_slist_peek(const struct mpmc_slist *q) {
    union mpmc_slist_head h = mpmc_slist_load(q);
    return h.node;
}

/* true if list was previously empty */
static inline bool mpmc_slist_push(struct mpmc_slist *q,
                                   struct mpmc_slist_node *n) {
    union mpmc_slist_head old = mpmc_slist_load(q);
    union mpmc_slist_head new_head;

    do {
        n->next = old.node;
        new_head.node = n;
        new_head.seq = old.seq + 1;
    } while (!mpmc_slist_cas(q, &old, new_head));

    return old.node == NULL;
}

/* Push a chain [first ... last] */
static inline bool mpmc_slist_push_chain(struct mpmc_slist *q,
                                         struct mpmc_slist_node *first,
                                         struct mpmc_slist_node *last) {
    union mpmc_slist_head old = mpmc_slist_load(q);
    union mpmc_slist_head new_head;

    do {
        last->next = old.node;
        new_head.node = first;
        new_head.seq = old.seq + 1;
    } while (!mpmc_slist_cas(q, &old, new_head));

    return old.node == NULL;
}

/* MC pop one node in LIFO order */
static inline struct mpmc_slist_node *mpmc_slist_pop(struct mpmc_slist *q) {
    union mpmc_slist_head old = mpmc_slist_load(q);
    union mpmc_slist_head new_head;

    while (old.node != NULL) {
        new_head.node = ca_read_once(old.node->next);
        new_head.seq = old.seq + 1;

        if (mpmc_slist_cas(q, &old, new_head))
            return old.node;
    }

    return NULL;
}

/* Return entire chain and mark empty */
static inline struct mpmc_slist_node *mpmc_slist_drain(struct mpmc_slist *q) {
    union mpmc_slist_head old = mpmc_slist_load(q);
    union mpmc_slist_head new_head;

    while (old.node != NULL) {
        new_head.node = NULL;
        new_head.seq = old.seq + 1;

        if (mpmc_slist_cas(q, &old, new_head))
            return old.node;
    }

    return NULL;
}

/* Get the FIFO order */
static inline struct mpmc_slist_node *
mpmc_slist_reverse(struct mpmc_slist_node *chain) {
    struct mpmc_slist_node *prev = NULL;
    while (chain) {
        struct mpmc_slist_node *next = chain->next;
        chain->next = prev;
        prev = chain;
        chain = next;
    }
    return prev;
}

#define mpmc_slist_entry(ptr, type, member) container_of(ptr, type, member)

#define mpmc_slist_for_each(pos, chain)                                        \
    for (pos = (chain); pos; pos = pos->next)

#define mpmc_slist_for_each_safe(pos, n, chain)                                \
    for (pos = (chain), n = (pos) ? (pos)->next : NULL; pos;                   \
         pos = n, n = (pos) ? (pos)->next : NULL)

#define mpmc_slist_for_each_entry(pos, chain, member)                          \
    for (pos = (chain) ? mpmc_slist_entry((chain), typeof(*pos), member)       \
                       : NULL;                                                 \
         pos;                                                                  \
         pos = (pos)->member.next ? mpmc_slist_entry((pos)->member.next,       \
                                                     typeof(*pos), member)     \
                                  : NULL)

#define mpmc_slist_for_each_entry_safe(pos, n, chain, member)                  \
    for (pos = (chain) ? mpmc_slist_entry((chain), typeof(*pos), member)       \
                       : NULL,                                                 \
        n = (pos && (pos)->member.next)                                        \
                ? mpmc_slist_entry((pos)->member.next, typeof(*pos), member)   \
                : NULL;                                                        \
         pos; pos = n,                                                         \
        n = (pos && (pos)->member.next)                                        \
                  ? mpmc_slist_entry((pos)->member.next, typeof(*pos), member) \
                  : NULL)
