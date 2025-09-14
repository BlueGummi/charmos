#include "internal.h"

struct worklist *worklist_create(enum worklist_flags flags) {
    struct worklist *ret = kmalloc(sizeof(struct worklist));
    INIT_LIST_HEAD(&ret->list);
    ret->creation_time = time_get_ms();
    ret->flags = flags;
    ret->state = WORKLIST_STATE_EMPTY;
    spinlock_init(&ret->lock);
    refcount_init(&ret->refcount, 1);
    return ret;
}

void worklist_destroy(struct worklist *list) {
    kfree(list);
}

static enum worklist_state worklist_change_state(struct worklist *wlist,
                                                 enum worklist_state new) {
    enum worklist_state old = atomic_load(&wlist->state);
    if (old < WORKLIST_STATE_RUNNING)
        atomic_store(&wlist->state, new);

    return old;
}

static void worklist_add_work(struct worklist *list, struct work *task) {
    enum irql irql = worklist_lock_irq_disable(list);
    list_add_tail(&task->list_node, &list->list);
    worklist_change_state(list, WORKLIST_STATE_READY);
    worklist_unlock(list, irql);
}

static void worklist_remove_work(struct worklist *list, struct work *task) {
    enum irql irql = worklist_lock_irq_disable(list);
    list_del(&task->list_node);

    if (list_empty(&list->list))
        worklist_change_state(list, WORKLIST_STATE_EMPTY);

    worklist_unlock(list, irql);
}

struct work *worklist_pop_front(struct worklist *list) {
    enum irql irql = worklist_lock_irq_disable(list);
    struct list_head *node = list_pop_front(&list->list);

    if (list_empty(&list->list))
        worklist_change_state(list, WORKLIST_STATE_EMPTY);

    worklist_unlock(list, irql);
    return work_from_worklist_node(node);
}

static bool worklist_empty(struct worklist *list) {
    enum irql irql = worklist_lock_irq_disable(list);
    bool empty = list_empty(&list->list);
    worklist_unlock(list, irql);
    return empty;
}

static void worklist_execute_internal(struct workqueue *queue,
                                      struct worklist *list) {
    struct list_head *iter;
    struct list_head *works = &list->list;
    list_for_each(iter, works) {
        struct work *work = work_from_worklist_node(iter);

        while (workqueue_enqueue_task(queue, work->func, work->arg,
                                      work->arg2) == WORKQUEUE_ERROR_FULL)
            scheduler_yield();
    }
}

void worklist_cancel_work(struct worklist *wl, struct work *wlw) {
    worklist_remove_work(wl, wlw);
}

enum workqueue_error worklist_execute(struct workqueue *queue,
                                      struct worklist *wlist) {
    if (atomic_exchange(&wlist->state, WORKLIST_STATE_RUNNING) ==
            WORKLIST_STATE_RUNNING &&
        !(wlist->flags & WORKLIST_FLAG_UNBOUND))
        return WORKQUEUE_ERROR_OK; /* OK - Already running and serialized */

    worklist_execute_internal(queue, wlist);

    return WORKQUEUE_ERROR_OK;
}
