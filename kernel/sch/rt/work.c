#include <thread/workqueue.h>

#include "internal.h"

struct workqueue *rt_wq = NULL;
void rt_scheduler_static_destroy_work_enqueue(struct rt_scheduler_static *rts) {

}

void rt_scheduler_workqueue_init() {
    if (!(rt_wq = workqueue_create_default("rt_wq")))
        panic("OOM\n");
}
