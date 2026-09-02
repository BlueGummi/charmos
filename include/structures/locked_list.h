/* @title: Locked list */
#pragma once
#include <kassert.h>
#include <structures/list.h>
#include <sync/spinlock.h>

/* Doubly linked list with a lock and counter for elements */

struct locked_list {
    struct list_head list;
    struct spinlock lock;
    size_t num_elems : 63;
    bool lock_irq_disable : 1;
};

static inline enum irql locked_list_lock(struct locked_list *ll) {
    if (ll->lock_irq_disable) {
        return spin_lock_irq_disable(&ll->lock);
    } else {
        return spin_lock(&ll->lock);
    }
}

static inline void locked_list_unlock(struct locked_list *ll, enum irql irql) {
    spin_unlock(&ll->lock, irql);
}

#define LOCKED_LIST_INIT(ll, irq_disable)                                      \
    (struct locked_list) {                                                     \
        .list = LIST_HEAD_INIT(ll.list), .lock = SPINLOCK_INIT,                \
        .num_elems = ATOMIC_VAR_INIT(0), .lock_irq_disable = irq_disable       \
    }

#define LOCKED_LIST_DEFINE(name, irq_disable)                                  \
    struct locked_list name = LOCKED_LIST_INIT(name, irq_disable)

#define LOCKED_LIST_INIT_IRQ_DISABLE true
#define LOCKED_LIST_INIT_NORMAL false
#define LOCKED_LIST_DO(ll, action)                                             \
    enum irql __macro_irql = locked_list_lock(ll);                             \
    action;                                                                    \
    locked_list_unlock(ll, __macro_irql);

static inline bool locked_list_empty(struct locked_list *ll) {
    LOCKED_LIST_DO(ll, bool empty = list_empty(&ll->list));
    return empty;
}

static inline void locked_list_add(struct locked_list *ll,
                                   struct list_head *lh) {
    LOCKED_LIST_DO(ll, list_add(lh, &ll->list); ll->num_elems++;);
}

static inline void locked_list_del(struct locked_list *ll,
                                   struct list_head *lh) {
    LOCKED_LIST_DO(ll, list_del_init(lh); ll->num_elems--;);
}

static inline void locked_list_del_locked(struct locked_list *ll,
                                          struct list_head *lh) {
    kassert(spinlock_locked(&ll->lock));
    list_del_init(lh);
    ll->num_elems--;
}

static inline struct list_head *locked_list_pop_front(struct locked_list *ll) {
    LOCKED_LIST_DO(ll, struct list_head *ret = list_pop_front(&ll->list));
    return ret;
}

static inline size_t locked_list_num_elems(struct locked_list *ll) {
    LOCKED_LIST_DO(ll, size_t ret = ll->num_elems);
    return ret;
}

static inline void locked_list_init(struct locked_list *ll, bool irq_disable) {
    INIT_LIST_HEAD(&ll->list);
    spinlock_init(&ll->lock);
    ll->num_elems = 0;
    ll->lock_irq_disable = irq_disable;
}
