#include "internal.h"

static atomic_uint_fast64_t worklist_id = 0;
struct work_list *work_list_create(void) {
    struct work_list *ret = kmalloc(sizeof(struct work_list));
    INIT_LIST_HEAD(&ret->worklist_node);
    INIT_LIST_HEAD(&ret->list);
    ret->creation_time = time_get_ms();
    spinlock_init(&ret->lock);
    ret->work_list_id = atomic_fetch_add(&worklist_id, 1);
    return ret;
}

void work_list_destroy(struct work_list *list) {
    kfree(list);
}

void work_list_add_to_queue(struct workqueue *queue, struct work_list *list) {
    enum irql irql = workqueue_lock_irq_disable(queue);
    list_add(&list->worklist_node, &queue->worklist_list);
    workqueue_unlock(queue, irql);
}

void work_list_remove_from_queue(struct workqueue *queue,
                                 struct work_list *list) {
    enum irql irql = workqueue_lock_irq_disable(queue);
    list_del(&list->worklist_node);
    workqueue_unlock(queue, irql);
}

void work_list_add_work(struct work_list *list, struct work *task) {
    enum irql irql = work_list_lock_irq_disable(list);
    list_add_tail(&task->list_node, &list->list);
    work_list_unlock(list, irql);
}

void work_list_remove_work(struct work_list *list, struct work *task) {
    enum irql irql = work_list_lock_irq_disable(list);
    list_del(&task->list_node);
    work_list_unlock(list, irql);
}

struct work *work_list_pop_front(struct work_list *list) {
    enum irql irql = work_list_lock_irq_disable(list);
    struct list_head *node = list_pop_front(&list->list);
    work_list_unlock(list, irql);
    return work_from_work_list_node(node);
}

static bool work_list_empty(struct work_list *list) {
    enum irql irql = work_list_lock_irq_disable(list);
    bool empty = list_empty(&list->list);
    work_list_unlock(list, irql);
    return empty;
}

void work_list_execute(struct work_list *list) {
    while (!work_list_empty(list))
        work_execute(work_list_pop_front(list));
}
