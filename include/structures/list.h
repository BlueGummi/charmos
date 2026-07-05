/* @title: Linked list */
#pragma once
#include <compiler.h>
#include <container_of.h>
#include <stddef.h>

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) {&(name), &(name)}

#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new, struct list_head *prev,
                              struct list_head *next) {
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head) {
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new,
                                 struct list_head *head) {
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry) {
    list_del(entry);
    INIT_LIST_HEAD(entry);
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

static inline struct list_head *list_peek_front(struct list_head *head) {
    if (list_empty(head))
        return NULL;

    return head->next;
}

static inline struct list_head *list_peek_tail(struct list_head *head) {
    if (list_empty(head))
        return NULL;

    return head->prev;
}

static inline struct list_head *list_pop_front(struct list_head *head) {
    if (list_empty(head))
        return NULL;

    struct list_head *entry = head->next;
    list_del(entry);
    return entry;
}

static inline struct list_head *list_pop_front_init(struct list_head *head) {
    struct list_head *ret = list_pop_front(head);
    if (ret)
        INIT_LIST_HEAD(ret);

    return ret;
}

static inline struct list_head *list_pop_tail(struct list_head *head) {
    if (list_empty(head))
        return NULL;

    struct list_head *entry = head->prev;
    list_del(entry);
    return entry;
}

static inline struct list_head *list_pop_tail_init(struct list_head *head) {
    struct list_head *ret = list_pop_tail(head);
    if (ret)
        INIT_LIST_HEAD(ret);

    return ret;
}

static inline void __list_splice(const struct list_head *list,
                                 struct list_head *prev,
                                 struct list_head *next) {
    struct list_head *first = list->next;
    struct list_head *last = list->prev;

    first->prev = prev;
    prev->next = first;

    last->next = next;
    next->prev = last;
}

static inline void list_splice_init(struct list_head *src,
                                    struct list_head *dst) {
    if (!list_empty(src)) {
        struct list_head *first = src->next;
        struct list_head *last = src->prev;
        struct list_head *at = dst;

        first->prev = at->prev;
        at->prev->next = first;

        last->next = at;
        at->prev = last;

        INIT_LIST_HEAD(src);
    }
}

static inline void list_splice_tail_init(struct list_head *list,
                                         struct list_head *head) {
    if (!list_empty(list)) {
        __list_splice(list, head->prev, head);
        INIT_LIST_HEAD(list);
    }
}

#define list_entry(ptr, type, member)                                          \
    ((type *) ((char *) (ptr) - (offsetof(type, member))))

#define list_for_each(pos, head)                                               \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head)                                       \
    for (pos = (head)->next, n = pos->next; pos != (head);                     \
         pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member)                                 \
    for (pos = list_entry((head)->next, typeof(*pos), member);                 \
         &pos->member != (head);                                               \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_first_entry(ptr, type, member)                                    \
    list_entry((ptr)->next, type, member)

#define list_for_each_entry_safe(pos, n, head, member)                         \
    for (pos = list_entry((head)->next, typeof(*pos), member),                 \
        n = list_entry(pos->member.next, typeof(*pos), member);                \
         &pos->member != (head);                                               \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

#define list_for_each_entry_safe_continue(pos, n, head, member)                \
    for (pos = list_entry(pos->member.next, typeof(*pos), member),             \
        n = list_entry(pos->member.next, typeof(*pos), member);                \
         &pos->member != (head);                                               \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

void list_sort(struct list_head *head,
               int (*cmp)(struct list_head *, struct list_head *));

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
