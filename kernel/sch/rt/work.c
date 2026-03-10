#include <thread/workqueue.h>

#include "internal.h"

struct workqueue *rt_wq = NULL;

static void destroy_work(void *a, void *b) {
    (void) b;
    rt_sched_trace("Destroying realtime scheduler %p", a);
    struct rt_scheduler_static *rts = a;
    /* UNLOADED! */
}

void rt_scheduler_static_destroy_work_enqueue(struct rt_scheduler_static *rts) {
    struct work *this_work = &rts->teardown_work;
    workqueue_enqueue(rt_wq, this_work);
}

void rt_scheduler_workqueue_init() {
    if (!(rt_wq = workqueue_create_default("rt_wq")))
        panic("OOM\n");
}

void rt_scheduler_static_work_init(struct rt_scheduler_static *rts) {
    struct work *this_work = &rts->teardown_work;
    work_init(this_work, destroy_work, WORK_ARGS(rts, NULL));
}
