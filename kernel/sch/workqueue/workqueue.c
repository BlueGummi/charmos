#include <compiler.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

#include "internal.h"

/* Array of pointers to wqs */
struct workqueue **workqueues = NULL;
int64_t num_workqueues = 0;

bool workqueue_add(dpc_t func, void *arg, void *arg2) {
    struct workqueue *queue = workqueue_get_least_loaded();
    return workqueue_enqueue_task(queue, func, arg, arg2);
}

bool workqueue_add_remote(dpc_t func, void *arg, void *arg2) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue_task(queue, func, arg, arg2);
}

bool workqueue_add_local(dpc_t func, void *arg, void *arg2) {
    struct workqueue *queue = workqueue_local();
    return workqueue_enqueue_task(queue, func, arg, arg2);
}

bool workqueue_add_fast(dpc_t func, void *arg, void *arg2) {
    struct workqueue *queue =
        workqueues[(get_this_core_id() + 1) % global.core_count];
    return workqueue_enqueue_task(queue, func, arg, arg2);
}
