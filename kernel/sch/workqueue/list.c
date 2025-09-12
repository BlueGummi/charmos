#include "internal.h"

static atomic_uint worklist_id = 0;
struct work_list *work_list_create(void) {
    struct work_list *ret = kmalloc(sizeof(struct work_list));
    INIT_LIST_HEAD(&ret->worklist_node);
    INIT_LIST_HEAD(&ret->list);
    ret->creation_time = time_get_ms();
    spinlock_init(&ret->lock);
    ret->work_list_id = worklist_id++;
    return ret;
}
