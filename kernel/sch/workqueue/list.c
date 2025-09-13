#include "internal.h"

struct worklist *work_list_create(void) {
    struct worklist *ret = kmalloc(sizeof(struct worklist));
    INIT_LIST_HEAD(&ret->worklist_node);
    INIT_LIST_HEAD(&ret->list);
    ret->creation_time = time_get_ms();
    spinlock_init(&ret->lock);
    return ret;
}

void work_list_destroy(struct worklist *list) {
    kfree(list);
}

void work_list_add_to_queue(struct workqueue *queue, struct worklist *list) {
    enum irql irql = workqueue_lock_irq_disable(queue);
    list_add(&list->worklist_node, &queue->worklist_list);
    list->workqueue = queue;
    workqueue_unlock(queue, irql);
}

void work_list_remove_from_queue(struct workqueue *queue,
                                 struct worklist *list) {
    enum irql irql = workqueue_lock_irq_disable(queue);
    list_del(&list->worklist_node);
    list->workqueue = NULL;
    workqueue_unlock(queue, irql);
}

void worklist_add_work(struct worklist *list, struct work *task) {
    enum irql irql = worklist_lock_irq_disable(list);
    list_add_tail(&task->list_node, &list->list);
    worklist_unlock(list, irql);
}

void worklist_remove_work(struct worklist *list, struct work *task) {
    enum irql irql = worklist_lock_irq_disable(list);
    list_del(&task->list_node);
    worklist_unlock(list, irql);
}

struct work *worklist_pop_front(struct worklist *list) {
    enum irql irql = worklist_lock_irq_disable(list);
    struct list_head *node = list_pop_front(&list->list);
    worklist_unlock(list, irql);
    return work_from_worklist_node(node);
}

static bool worklist_empty(struct worklist *list) {
    enum irql irql = worklist_lock_irq_disable(list);
    bool empty = list_empty(&list->list);
    worklist_unlock(list, irql);
    return empty;
}

void worklist_execute(struct worklist *list) {
    while (!worklist_empty(list))
        work_execute(worklist_pop_front(list));
}
