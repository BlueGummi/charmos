/* @title: Lockless MPMC LIFO list */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types/types.h>

struct mpmc_lifo_list_node {
    struct mpmc_lifo_list_node *next;
};

union mpmc_lifo_list_head {
    struct {
        struct mpmc_lifo_list_node *node;
        uint64_t seq;
    };
    uint128_t raw;
};

struct mpmc_lifo_list {
    atomic_uint128_t head;
};

#define MPMC_LIFO_LIST_INIT {.head = 0}
#define MPMC_LIFO_LIST(name) struct mpmc_lifo_list name = MPMC_LIFO_LIST_INIT

/* NOTE: my syntax highlighter breaks when I actually insert
 * the keyword struct/union into the full versions, not sure why */
ct_assert_union_size_eq(mpmc_lifo_list_head, 16);
ct_assert_struct_size_eq(mpmc_lifo_list, 16);
ct_assert_struct_align_eq(mpmc_lifo_list, 16);

static inline bool mpmc_lifo_list_cas(struct mpmc_lifo_list *q,
                                      union mpmc_lifo_list_head *expected,
                                      union mpmc_lifo_list_head desired) {
    return atomic_cas_strong(&q->head, &expected->raw, desired.raw, mo_acq_rel,
                             mo_acquire);
}

static inline union mpmc_lifo_list_head
mpmc_lifo_list_load(const struct mpmc_lifo_list *q) {
    return (union mpmc_lifo_list_head) {.raw = atomic_load_acq(&q->head)};
}

static inline void mpmc_lifo_list_init(struct mpmc_lifo_list *q) {
    atomic_store_rlx(&q->head, 0);
}

static inline bool mpmc_lifo_list_empty(const struct mpmc_lifo_list *q) {
    union mpmc_lifo_list_head h = mpmc_lifo_list_load(q);
    return h.node == NULL;
}

static inline uint64_t mpmc_lifo_list_seq(const struct mpmc_lifo_list *q) {
    union mpmc_lifo_list_head h = mpmc_lifo_list_load(q);
    return h.seq;
}

static inline struct mpmc_lifo_list_node *
mpmc_lifo_list_peek(const struct mpmc_lifo_list *q) {
    union mpmc_lifo_list_head h = mpmc_lifo_list_load(q);
    return h.node;
}

/* true if list was previously empty */
static inline bool mpmc_lifo_list_push(struct mpmc_lifo_list *q,
                                       struct mpmc_lifo_list_node *n) {
    union mpmc_lifo_list_head old = mpmc_lifo_list_load(q);
    union mpmc_lifo_list_head new_head;

    do {
        n->next = old.node;
        new_head.node = n;
        new_head.seq = old.seq + 1;
    } while (!mpmc_lifo_list_cas(q, &old, new_head));

    return old.node == NULL;
}

/* Push a chain [first ... last] */
static inline bool mpmc_lifo_list_push_chain(struct mpmc_lifo_list *q,
                                             struct mpmc_lifo_list_node *first,
                                             struct mpmc_lifo_list_node *last) {
    union mpmc_lifo_list_head old = mpmc_lifo_list_load(q);
    union mpmc_lifo_list_head new_head;

    do {
        last->next = old.node;
        new_head.node = first;
        new_head.seq = old.seq + 1;
    } while (!mpmc_lifo_list_cas(q, &old, new_head));

    return old.node == NULL;
}

/* MC pop one node in LIFO order */
static inline struct mpmc_lifo_list_node *
mpmc_lifo_list_pop(struct mpmc_lifo_list *q) {
    union mpmc_lifo_list_head old = mpmc_lifo_list_load(q);
    union mpmc_lifo_list_head new_head;

    while (old.node != NULL) {
        new_head.node = ca_read_once(old.node->next);
        new_head.seq = old.seq + 1;

        if (mpmc_lifo_list_cas(q, &old, new_head))
            return old.node;
    }

    return NULL;
}

/* Return entire chain and mark empty */
static inline struct mpmc_lifo_list_node *
mpmc_lifo_list_drain(struct mpmc_lifo_list *q) {
    union mpmc_lifo_list_head old = mpmc_lifo_list_load(q);
    union mpmc_lifo_list_head new_head;

    while (old.node != NULL) {
        new_head.node = NULL;
        new_head.seq = old.seq + 1;

        if (mpmc_lifo_list_cas(q, &old, new_head))
            return old.node;
    }

    return NULL;
}

/* Get the FIFO order */
static inline struct mpmc_lifo_list_node *
mpmc_lifo_list_reverse(struct mpmc_lifo_list_node *chain) {
    struct mpmc_lifo_list_node *prev = NULL;
    while (chain) {
        struct mpmc_lifo_list_node *next = chain->next;
        chain->next = prev;
        prev = chain;
        chain = next;
    }
    return prev;
}

#define mpmc_lifo_list_entry(ptr, type, member) container_of(ptr, type, member)

#define mpmc_lifo_list_for_each(pos, chain)                                    \
    for (pos = (chain); pos; pos = pos->next)

#define mpmc_lifo_list_for_each_safe(pos, n, chain)                            \
    for (pos = (chain), n = (pos) ? (pos)->next : NULL; pos;                   \
         pos = n, n = (pos) ? (pos)->next : NULL)

#define mpmc_lifo_list_for_each_entry(pos, chain, member)                      \
    for (pos = (chain) ? mpmc_lifo_list_entry((chain), typeof(*pos), member)   \
                       : NULL;                                                 \
         pos;                                                                  \
         pos = (pos)->member.next ? mpmc_lifo_list_entry((pos)->member.next,   \
                                                         typeof(*pos), member) \
                                  : NULL)

#define mpmc_lifo_list_for_each_entry_safe(pos, n, chain, member)              \
    for (pos = (chain) ? mpmc_lifo_list_entry((chain), typeof(*pos), member)   \
                       : NULL,                                                 \
        n = (pos && (pos)->member.next)                                        \
                ? mpmc_lifo_list_entry((pos)->member.next, typeof(*pos),       \
                                       member)                                 \
                : NULL;                                                        \
         pos; pos = n, n = (pos && (pos)->member.next)                         \
                               ? mpmc_lifo_list_entry((pos)->member.next,      \
                                                      typeof(*pos), member)    \
                               : NULL)
