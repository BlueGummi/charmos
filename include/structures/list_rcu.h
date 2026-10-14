/* @title: RCU list operations */
#pragma once
#include <compiler/atomic.h>
#include <compiler/core.h>
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>
#include <structures/hlist.h>
#include <structures/list.h>
#include <sync/rcu.h>

static inline void list_node_init_rcu(struct list_node *n) {
    ca_write_once(n->next, n);
    ca_write_once(n->prev, n);
}

static inline void list_head_init_rcu(struct list_head *h) {
    list_node_init_rcu(&h->root);
}

#define list_next_rcu(list) (*((struct list_node **) &(list)->next))
#define list_bidir_prev_rcu(list) (*((struct list_node **) &(list)->prev))
#define list_tail_rcu(head) (*((struct list_node **) &(list_root(head))->prev))

static inline void list_node_link_between_rcu(struct list_node *new,
                                              struct list_node *prev,
                                              struct list_node *next) {
    list_add_check(new, prev, next);

    new->next = next;
    new->prev = prev;
    rcu_assign_pointer(list_next_rcu(prev), new);
    next->prev = new;
}

static inline void list_head_push_front_rcu(struct list_head *h,
                                            struct list_node *n) {
    list_node_link_between_rcu(n, &h->root, h->root.next);
}

static inline void list_head_push_back_rcu(struct list_head *h,
                                           struct list_node *n) {
    list_node_link_between_rcu(n, h->root.prev, &h->root);
}

/* TODO: Consider poisoning (not just NULL) and also maybe using
 * an address_space for RCU lists */
static inline void list_node_delete_rcu(struct list_node *n) {
    list_del_check(n);

    list_node_unlink_between(n->prev, n->next);
    n->prev = NULL;
}

static inline void list_node_delete_bidir_rcu(struct list_node *n) {
    list_del_check(n);

    list_node_unlink_between(n->prev, n->next);
}

static inline void list_node_replace_rcu(struct list_node *old,
                                         struct list_node *new) {
    new->next = old->next;
    new->prev = old->prev;
    rcu_assign_pointer(list_next_rcu(new->prev), new);
    new->next->prev = new;
    old->prev = NULL;
}

static inline void list_splice_between_rcu(const struct list_node *list,
                                           struct list_node *prev,
                                           struct list_node *next) {
    list_splice_check(list, prev, next);

    struct list_node *first = list->next;
    struct list_node *last = list->prev;

    last->next = next;
    first->prev = prev;
    next->prev = last;
    rcu_assign_pointer(list_next_rcu(prev), first);
}

/* copy src onto the front of dst */
static inline void list_head_splice_front_rcu(struct list_head *dst,
                                              struct list_head *src) {
    if (!list_head_empty(src))
        list_splice_between_rcu(&src->root, &dst->root, dst->root.next);
}

static inline void list_splice_between_init_rcu(struct list_node *list,
                                                struct list_node *prev,
                                                struct list_node *next,
                                                void (*sync)(void)) {
    struct list_node *first = list->next;
    struct list_node *last = list->prev;

    list_node_init_rcu(list);

    if (sync)
        sync();

    last->next = next;
    rcu_assign_pointer(list_next_rcu(prev), first);
    first->prev = prev;
    next->prev = last;
}

static inline void list_head_splice_front_init_rcu(struct list_head *dst,
                                                   struct list_head *src,
                                                   void (*sync)(void)) {
    if (!list_head_empty(src))
        list_splice_between_init_rcu(&src->root, &dst->root, dst->root.next,
                                     sync);
}

static inline void list_head_splice_back_init_rcu(struct list_head *dst,
                                                  struct list_head *src,
                                                  void (*sync)(void)) {
    if (!list_head_empty(src))
        list_splice_between_init_rcu(&src->root, dst->root.prev, &dst->root,
                                     sync);
}

#define list_entry_rcu(ptr, type, member)                                      \
    container_of(rcu_dereference(ptr), type, member)

#define list_first_or_null_rcu(ptr, type, member)                              \
    ({                                                                         \
        __auto_type __ptr = list_root(ptr);                                    \
        struct list_node *__next = rcu_dereference(__ptr->next);               \
        cc_likely(__ptr != __next) ? list_entry(__next, type, member) : NULL;  \
    })

#define list_next_or_null_rcu(head, ptr, type, member)                         \
    ({                                                                         \
        __auto_type __head = list_root(head);                                  \
        struct list_node *__ptr = (ptr);                                       \
        struct list_node *__next = rcu_dereference(__ptr->next);               \
        cc_likely(__next != __head) ? list_entry(__next, type, member) : NULL; \
    })

#define list_for_each_rcu(pos, head)                                           \
    for (pos = rcu_dereference(list_root(head)->next); pos != list_root(head); \
         pos = rcu_dereference(pos->next))

#define list_for_each_entry_rcu(pos, head, member)                             \
    for (pos = list_entry_rcu(list_root(head)->next, typeof(*pos), member);    \
         &pos->member != list_root(head);                                      \
         pos = list_entry_rcu(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_continue_rcu(pos, head, member)                    \
    for (pos = list_entry_rcu(pos->member.next, typeof(*pos), member);         \
         &pos->member != list_root(head);                                      \
         pos = list_entry_rcu(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_from_rcu(pos, head, member)                        \
    for (; &pos->member != list_root(head);                                    \
         pos = list_entry_rcu(pos->member.next, typeof(*pos), member))

#define hlist_first_rcu(head) (*((struct hlist_node **) &(head)->first))
#define hlist_next_rcu(node) (*((struct hlist_node **) &(node)->next))
#define hlist_pprev_rcu(node) (*((struct hlist_node **) ((node)->pprev)))

#define hlist_entry_rcu(ptr, type, member)                                     \
    container_of(rcu_dereference(ptr), type, member)

static inline void hlist_node_unlink(struct hlist_node *n) {
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;

    *pprev = next;
    if (next)
        next->pprev = pprev;
}

static inline void hlist_del_rcu(struct hlist_node *n) {
    hlist_node_unlink(n);
    n->pprev = NULL;
}

static inline void hlist_del_init_rcu(struct hlist_node *n) {
    if (!hlist_unhashed(n)) {
        hlist_node_unlink(n);
        ca_write_once(n->pprev, NULL);
    }
}

static inline void hlist_replace_rcu(struct hlist_node *old,
                                     struct hlist_node *new) {
    struct hlist_node *next = old->next;

    new->next = next;
    new->pprev = old->pprev;
    rcu_assign_pointer(*(struct hlist_node **) new->pprev, new);
    if (next)
        new->next->pprev = &new->next;
    old->pprev = NULL;
}

static inline void hlists_swap_heads_rcu(struct hlist_head *left,
                                         struct hlist_head *right) {
    struct hlist_node *node1 = left->first;
    struct hlist_node *node2 = right->first;

    rcu_assign_pointer(left->first, node2);
    rcu_assign_pointer(right->first, node1);
    if (node2)
        ca_write_once(node2->pprev, &left->first);
    if (node1)
        ca_write_once(node1->pprev, &right->first);
}

static inline void hlist_add_head_rcu(struct hlist_node *n,
                                      struct hlist_head *h) {
    struct hlist_node *first = h->first;

    n->next = first;
    n->pprev = &h->first;
    rcu_assign_pointer(hlist_first_rcu(h), n);
    if (first)
        first->pprev = &n->next;
}

static inline void hlist_add_tail_rcu(struct hlist_node *n,
                                      struct hlist_head *h) {
    struct hlist_node *i, *last = NULL;

    for (i = h->first; i; i = i->next)
        last = i;

    if (last) {
        n->next = last->next;
        n->pprev = &last->next;
        rcu_assign_pointer(hlist_next_rcu(last), n);
    } else {
        hlist_add_head_rcu(n, h);
    }
}

static inline void hlist_add_before_rcu(struct hlist_node *n,
                                        struct hlist_node *next) {
    n->pprev = next->pprev;
    n->next = next;
    rcu_assign_pointer(hlist_pprev_rcu(n), n);
    next->pprev = &n->next;
}

static inline void hlist_add_behind_rcu(struct hlist_node *n,
                                        struct hlist_node *prev) {
    n->next = prev->next;
    n->pprev = &prev->next;
    rcu_assign_pointer(hlist_next_rcu(prev), n);
    if (n->next)
        n->next->pprev = &n->next;
}

#define hlist_for_each_node_rcu(pos, head)                                     \
    for (pos = rcu_dereference(hlist_first_rcu(head)); pos;                    \
         pos = rcu_dereference(hlist_next_rcu(pos)))

#define hlist_for_each_entry_rcu(pos, head, member)                            \
    for (pos = hlist_entry_safe(rcu_dereference(hlist_first_rcu(head)),        \
                                typeof(*(pos)), member);                       \
         pos; pos = hlist_entry_safe(                                          \
                  rcu_dereference(hlist_next_rcu(&(pos)->member)),             \
                  typeof(*(pos)), member))

#define hlist_for_each_entry_continue_rcu(pos, member)                         \
    for (pos =                                                                 \
             hlist_entry_safe(rcu_dereference(hlist_next_rcu(&(pos)->member)), \
                              typeof(*(pos)), member);                         \
         pos; pos = hlist_entry_safe(                                          \
                  rcu_dereference(hlist_next_rcu(&(pos)->member)),             \
                  typeof(*(pos)), member))

#define hlist_for_each_entry_from_rcu(pos, member)                             \
    for (; pos; pos = hlist_entry_safe(                                        \
                    rcu_dereference(hlist_next_rcu(&(pos)->member)),           \
                    typeof(*(pos)), member))
