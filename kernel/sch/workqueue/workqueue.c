#include <compiler.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

#include "internal.h"

enum workqueue_error workqueue_add(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded();
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

enum workqueue_error workqueue_add_remote(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

enum workqueue_error workqueue_add_local(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_local();
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

enum workqueue_error workqueue_add_fast(dpc_t func, struct work_args args) {
    struct workqueue *queue =
        global.workqueues[(get_this_core_id() + 1) % global.core_count];
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

void work_execute(struct work *task) {
    if (!task)
        return;

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    task->func(task->arg, task->arg2);
    irql_lower(old);
}
