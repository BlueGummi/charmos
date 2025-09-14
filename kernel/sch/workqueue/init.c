#include "internal.h"
#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>

struct worker *workqueue_spawn_initial_worker(struct workqueue *queue,
                                              int64_t core) {
    struct thread *thread;

    if (WORKQUEUE_FLAG_SET(queue, WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS))
        thread = worker_create_unmigratable();
    else
        thread = worker_create();

    if (!thread)
        return NULL;

    struct worker *worker = kzalloc(sizeof(struct worker));
    if (!worker)
        return NULL;

    INIT_LIST_HEAD(&worker->list_node);

    if (!WORKQUEUE_FLAG_SET(queue, WORKQUEUE_FLAG_ALLOW_NO_WORKERS))
        worker->is_permanent = true;

    worker->inactivity_check_period = queue->attrs.inactive_check_period.max;
    worker->workqueue = queue;

    workqueue_link_thread_and_worker(worker, thread);

    if (core != -1)
        scheduler_enqueue_on_core(thread, core);
    else
        scheduler_enqueue(thread);

    workqueue_update_queue_after_spawn(queue);

    workqueue_add_worker(queue, worker);

    return worker;
}

void workqueues_permanent_init(void) {
    int64_t num_workqueues = global.core_count;
    global.workqueues = kzalloc(sizeof(struct workqueue *) * num_workqueues);

    if (!global.workqueues)
        k_panic("Failed to allocate space for workqueues!\n");

    for (int64_t i = 0; i < num_workqueues; ++i) {

        struct workqueue_attributes attrs = {
            .capacity = DEFAULT_WORKQUEUE_CAPACITY,
            .max_workers = DEFAULT_MAX_WORKERS,
            .spawn_delay = DEFAULT_SPAWN_DELAY,
            .inactive_check_period.min = DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD,
            .inactive_check_period.max = DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD,
            .flags = WORKQUEUE_FLAG_PERMANENT | WORKQUEUE_FLAG_AUTO_SPAWN |
                     WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS,
        };

        global.workqueues[i] = workqueue_create_internal(&attrs);

        if (!global.workqueues[i])
            k_panic("Failed to spawn permanent workqueue\n");

        if (!workqueue_spawn_initial_worker(global.workqueues[i], i))
            k_panic("Failed to spawn initial worker on workqueue %u\n", i);
    }
}
