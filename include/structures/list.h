/* @title: Linked list */
#pragma once
#include <compiler/core.h>
#include <container_of.h>
#include <stdbool.h>
#include <stddef.h>

/* Circular doubly linked list.
 *
 * We use two different types here since a unified list_head proved problematic
 * for readability and semantics. It's largely symbolic and for type safety.
 *
 * struct list_node is the link
 * struct list_head is the head/anchor of the list.
 *
 * list_head_* operations take the list_head,
 * list_node_* operations a list_node,
 * list_root_* operations technically take a list_node, but this is meant
 * to be the list_head's root member, and is used as convenience.
 *
 * Thus, swapping arguments like list_add(&parent->node, &child->node); is
 * not possible and fails to compile. */
struct list_node {
    struct list_node *next, *prev;
};

struct list_head {
    struct list_node root;
};

static_assert(sizeof(struct list_head) == sizeof(struct list_node));
static_assert(offsetof(struct list_head, root) == 0);

#ifdef DEBUG_LIST
void list_add_check(const struct list_node *new, const struct list_node *prev,
                    const struct list_node *next);
void list_del_check(const struct list_node *entry);
void list_splice_check(const struct list_node *list,
                       const struct list_node *prev,
                       const struct list_node *next);
#else
static inline void list_add_check(const struct list_node *new,
                                  const struct list_node *prev,
                                  const struct list_node *next) {
    cc_ignore(new, prev, next);
}

static inline void list_del_check(const struct list_node *entry) {
    cc_ignore(entry);
}

static inline void list_splice_check(const struct list_node *list,
                                     const struct list_node *prev,
                                     const struct list_node *next) {
    cc_ignore(list, prev, next);
}
#endif /* DEBUG_LIST */

#define LIST_NODE_INIT(name) {&(name), &(name)}

#define LIST_HEAD_INIT(name)                                                   \
    {                                                                          \
        .root = { &(name).root, &(name).root }                                 \
    }

#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

#define list_root(h) (&(h)->root)

static inline void list_node_init(struct list_node *n) {
    n->next = n;
    n->prev = n;
}

static inline void list_head_init(struct list_head *h) {
    list_node_init(&h->root);
}

static inline bool list_node_is_linked(const struct list_node *n) {
    return n->next != NULL && n->next != n;
}

static inline struct list_node *list_node_next(const struct list_node *n) {
    return n->next;
}

static inline struct list_node *list_node_prev(const struct list_node *n) {
    return n->prev;
}

static inline struct list_node *list_head_sentinel(struct list_head *h) {
    return &h->root;
}

/* Treat a node as if it's the head. Should only be used in headless
 * lists where one of the middle members is also the anchor */
static inline struct list_head *list_node_as_head(struct list_node *n) {
    /* container_of in here in case we want to do more tracking
     * and debugging in list_head */
    return container_of(n, struct list_head, root);
}

static inline void list_node_link_between(struct list_node *new,
                                          struct list_node *prev,
                                          struct list_node *next) {
    list_add_check(new, prev, next);

    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_node_unlink_between(struct list_node *prev,
                                            struct list_node *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_node_insert_after(struct list_node *pos,
                                          struct list_node *n) {
    list_node_link_between(n, pos, pos->next);
}

static inline void list_node_insert_before(struct list_node *pos,
                                           struct list_node *n) {
    list_node_link_between(n, pos->prev, pos);
}

static inline void list_node_delete(struct list_node *n) {
    list_del_check(n);

    list_node_unlink_between(n->prev, n->next);
    n->next = NULL;
    n->prev = NULL;
}

static inline void list_node_delete_init(struct list_node *n) {
    list_node_delete(n);
    list_node_init(n);
}

static inline bool list_head_empty(const struct list_head *h) {
    return h->root.next == &h->root;
}

static inline void list_head_push_front(struct list_head *h,
                                        struct list_node *n) {
    list_node_insert_after(&h->root, n);
}

static inline void list_head_push_back(struct list_head *h,
                                       struct list_node *n) {
    list_node_insert_before(&h->root, n);
}

/* root_*, For direct sentinel manipulation */
static inline struct list_node *list_root_pop_front(struct list_node *root) {
    if (root->next == root)
        return NULL;

    struct list_node *n = root->next;
    list_node_delete(n);
    return n;
}

static inline struct list_node *list_root_pop_back(struct list_node *root) {
    if (root->next == root)
        return NULL;

    struct list_node *n = root->prev;
    list_node_delete(n);
    return n;
}

static inline struct list_node *list_head_peek_front(struct list_head *h) {
    return list_head_empty(h) ? NULL : h->root.next;
}

static inline struct list_node *list_head_peek_back(struct list_head *h) {
    return list_head_empty(h) ? NULL : h->root.prev;
}

static inline struct list_node *list_head_pop_front(struct list_head *h) {
    return list_root_pop_front(&h->root);
}

static inline struct list_node *list_head_pop_back(struct list_head *h) {
    return list_root_pop_back(&h->root);
}

static inline struct list_node *list_head_pop_front_init(struct list_head *h) {
    struct list_node *n = list_head_pop_front(h);
    if (n)
        list_node_init(n);

    return n;
}

static inline struct list_node *list_head_pop_back_init(struct list_head *h) {
    struct list_node *n = list_head_pop_back(h);
    if (n)
        list_node_init(n);

    return n;
}

static inline void list_splice_between(const struct list_node *list,
                                       struct list_node *prev,
                                       struct list_node *next) {
    list_splice_check(list, prev, next);

    struct list_node *first = list->next;
    struct list_node *last = list->prev;

    first->prev = prev;
    prev->next = first;

    last->next = next;
    next->prev = last;
}

/* Move everyone from src to the front of dst */
static inline void list_head_splice_front_init(struct list_head *dst,
                                               struct list_head *src) {
    if (!list_head_empty(src)) {
        list_splice_between(&src->root, &dst->root, dst->root.next);
        list_head_init(src);
    }
}

/* Move everyone from src to the back of dst */
static inline void list_head_splice_back_init(struct list_head *dst,
                                              struct list_head *src) {
    if (!list_head_empty(src)) {
        list_splice_between(&src->root, dst->root.prev, &dst->root);
        list_head_init(src);
    }
}

typedef int (*list_cmp_fn)(struct list_node *, struct list_node *);

void list_root_sort(struct list_node *root, list_cmp_fn cmp);

static inline void list_head_sort(struct list_head *h, list_cmp_fn cmp) {
    list_root_sort(&h->root, cmp);
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_entry_safe(ptr, type, member)                                     \
    ({                                                                         \
        __auto_type __list_ptr = (ptr);                                        \
        __list_ptr ? list_entry(__list_ptr, type, member) : NULL;              \
    })

#define list_for_each(pos, head)                                               \
    for (pos = list_root(head)->next; pos != list_root(head); pos = pos->next)

#define list_for_each_rev(pos, head)                                           \
    for (pos = list_root(head)->prev; pos != list_root(head); pos = pos->prev)

#define list_for_each_safe(pos, n, head)                                       \
    for (pos = list_root(head)->next, n = pos->next; pos != list_root(head);   \
         pos = n, n = pos->next)

#define list_for_each_rev_safe(pos, n, head)                                   \
    for (pos = list_root(head)->prev, n = pos->prev; pos != list_root(head);   \
         pos = n, n = pos->prev)

#define list_for_each_entry(pos, head, member)                                 \
    for (pos = list_entry(list_root(head)->next, typeof(*pos), member);        \
         &pos->member != list_root(head);                                      \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_rev(pos, head, member)                             \
    for (pos = list_entry(list_root(head)->prev, typeof(*pos), member);        \
         &pos->member != list_root(head);                                      \
         pos = list_entry(pos->member.prev, typeof(*pos), member))

#define list_first_entry(ptr, type, member)                                    \
    list_entry(list_root(ptr)->next, type, member)

#define list_last_entry(ptr, type, member)                                     \
    list_entry(list_root(ptr)->prev, type, member)

#define list_for_each_entry_safe(pos, n, head, member)                         \
    for (pos = list_entry(list_root(head)->next, typeof(*pos), member),        \
        n = list_entry(pos->member.next, typeof(*pos), member);                \
         &pos->member != list_root(head);                                      \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

#define list_for_each_entry_safe_rev(pos, n, head, member)                     \
    for (pos = list_entry(list_root(head)->prev, typeof(*pos), member),        \
        n = list_entry(pos->member.prev, typeof(*pos), member);                \
         &pos->member != list_root(head);                                      \
         pos = n, n = list_entry(n->member.prev, typeof(*n), member))

#define list_for_each_entry_safe_continue(pos, n, head, member)                \
    for (pos = list_entry(pos->member.next, typeof(*pos), member),             \
        n = list_entry(pos->member.next, typeof(*pos), member);                \
         &pos->member != list_root(head);                                      \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

#define list_for_each_entry_safe_continue_rev(pos, n, head, member)            \
    for (pos = list_entry(pos->member.prev, typeof(*pos), member),             \
        n = list_entry(pos->member.prev, typeof(*pos), member);                \
         &pos->member != list_root(head);                                      \
         pos = n, n = list_entry(n->member.prev, typeof(*n), member))

struct slist_node {
    struct slist_node *next;
};

struct slist_head {
    struct slist_node *first;
    struct slist_node *last;
};

static inline void slist_head_init(struct slist_head *h) {
    h->first = NULL;
    h->last = NULL;
}

static inline int slist_empty(const struct slist_head *h) {
    return h->first == NULL;
}

static inline void slist_push_head(struct slist_head *h, struct slist_node *n) {
    n->next = h->first;
    h->first = n;
    if (h->last == NULL)
        h->last = n;
}

static inline void slist_push_tail(struct slist_head *h, struct slist_node *n) {
    n->next = NULL;

    if (h->last != NULL) {
        h->last->next = n;
    } else {
        h->first = n;
    }

    h->last = n;
}

static inline struct slist_node *slist_pop_head(struct slist_head *h) {
    struct slist_node *n = h->first;
    if (!n)
        return NULL;

    h->first = n->next;
    if (h->first == NULL)
        h->last = NULL;

    n->next = NULL;
    return n;
}

static inline void slist_remove_node_after(struct slist_head *h,
                                           struct slist_node *prev,
                                           struct slist_node *target) {

    if (!prev) {
        if (h->first != target)
            return;

        slist_pop_head(h);
        return;
    }

    if (prev->next != target)
        return;

    prev->next = target->next;

    if (h->last == target)
        h->last = prev;

    target->next = NULL;
}
