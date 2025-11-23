/* @title: Locked list */
#include <kassert.h>
#include <structures/list.h>
#include <sync/spinlock.h>

/* Doubly linked list with a lock and counter for elements */

struct locked_list {
    struct list_head list;
    struct spinlock lock;
    atomic_size_t num_elems;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(locked_list, lock);

#define LOCKED_LIST_SET_NUM_ELEMS(ll, c) atomic_store(&ll->num_elems, c)
#define LOCKED_LIST_GET_NUM_ELEMS(ll) atomic_load(&ll->num_elems)
#define LOCKED_LIST_INC_NUM_ELEMS(ll) atomic_fetch_add(&ll->num_elems, 1)
#define LOCKED_LIST_DEC_NUM_ELEMS(ll) atomic_fetch_sub(&ll->num_elems, 1)
#define LOCKED_LIST_DO(ll, action)                                             \
    enum irql __macro_irql = locked_list_lock_irq_disable(ll);                 \
    action;                                                                    \
    locked_list_unlock(ll, __macro_irql);

static inline bool locked_list_empty(struct locked_list *ll) {
    LOCKED_LIST_DO(ll, bool empty = list_empty(&ll->list));
    return empty;
}

static inline void locked_list_add(struct locked_list *ll,
                                   struct list_head *lh) {
    LOCKED_LIST_DO(ll, list_add(lh, &ll->list));
    LOCKED_LIST_INC_NUM_ELEMS(ll);
}

static inline void locked_list_del(struct locked_list *ll,
                                   struct list_head *lh) {
    LOCKED_LIST_DO(ll, list_del_init(lh));
    LOCKED_LIST_DEC_NUM_ELEMS(ll);
}

static inline void locked_list_del_locked(struct locked_list *ll,
                                          struct list_head *lh) {
    kassert(spinlock_held(&ll->lock));
    list_del_init(lh);
    LOCKED_LIST_DEC_NUM_ELEMS(ll);
}

static inline struct list_head *locked_list_pop_front(struct locked_list *ll) {
    LOCKED_LIST_DO(ll, struct list_head *ret = list_pop_front(&ll->list));
    return ret;
}

static inline size_t locked_list_num_elems(struct locked_list *ll) {
    LOCKED_LIST_DO(ll, size_t ret = LOCKED_LIST_GET_NUM_ELEMS(ll));
    return ret;
}

static inline void locked_list_init(struct locked_list *ll) {
    INIT_LIST_HEAD(&ll->list);
    spinlock_init(&ll->lock);
    LOCKED_LIST_SET_NUM_ELEMS(ll, 0);
}
