/* @title: Hash list */
#pragma once
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>

struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev;
};

#define HLIST_HEAD_INIT {.first = NULL}
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h) {
    h->next = NULL;
    h->pprev = NULL;
}

static inline bool hlist_unhashed(const struct hlist_node *h) {
    return !h->pprev;
}

static inline bool hlist_empty(const struct hlist_head *h) {
    return !h->first;
}

static inline void hlist_add_before(struct hlist_node *n,
                                    struct hlist_node *next) {
    n->pprev = next->pprev;
    n->next = next;
    *next->pprev = n;
    next->pprev = &n->next;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h) {
    struct hlist_node *first = h->first;
    n->next = first;
    if (first)
        first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}

static inline void hlist_del(struct hlist_node *n) {
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;

    *pprev = next;
    if (next)
        next->pprev = pprev;

    n->next = NULL;
    n->pprev = NULL;
}

static inline void hlist_move_list(struct hlist_head *old,
                                   struct hlist_head *new) {
    new->first = old->first;
    if (new->first)
        new->first->pprev = &new->first;
    old->first = NULL;
}

static inline struct hlist_node *hlist_pop_head(struct hlist_head *h) {
    struct hlist_node *first = h->first;
    if (first)
        hlist_del(first);
    return first;
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_for_each_entry(pos, head, member)                                \
    for (pos = hlist_entry((head)->first, typeof(*pos), member); pos;          \
         pos = hlist_entry(pos->member.next, typeof(*pos), member))
