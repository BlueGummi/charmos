#include "internal.h"
#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>

static void spawn_permanent_thread_on_core(uint64_t core) {
    struct workqueue *queue = global.workqueues[core];

    struct thread *thread = worker_create_unmigratable();
    if (!thread) {
        k_panic("Failed to spawn permanent worker thread on core %llu\n", core);
    }

    struct worker *worker = &queue->workers[0];
    worker->is_permanent = true;
    worker->inactivity_check_period = MINUTES_TO_MS(5);
    workqueue_link_thread_and_worker(worker, thread);
    scheduler_enqueue_on_core(thread, core);
    workqueue_update_queue_after_spawn(queue);
}

struct workqueue *workqueue_create(struct workqueue_attributes *attrs) {
    struct workqueue *ret = kzalloc(sizeof(struct workqueue));
    if (!ret)
        return NULL;

    spinlock_init(&ret->lock);
    ret->attrs = *attrs;
    ret->tasks = kzalloc(sizeof(struct work) * attrs->capacity);
    if (!ret->tasks) {
        kfree(ret);
        return NULL;
    }

    ret->workers = kzalloc(sizeof(struct worker) * attrs->max_workers);
    if (!ret->workers) {
        kfree(ret->tasks);
        kfree(ret);
        return NULL;
    }

    kassert(attrs->max_workers < 64);
    for (uint64_t i = 0; i < attrs->capacity; i++)
        atomic_store_explicit(&ret->tasks[i].seq, i, memory_order_relaxed);

    return ret;
}

void workqueues_permanent_init(void) {
    int64_t num_workqueues = global.core_count;
    global.workqueues = kzalloc(sizeof(struct workqueue *) * num_workqueues);

    if (!global.workqueues)
        k_panic("Failed to allocate space for workqueues!\n");

    for (int64_t i = 0; i < num_workqueues; ++i) {

        global.workqueues[i] = kzalloc(sizeof(struct workqueue));
        if (!global.workqueues[i])
            k_panic("Failed to allocate space for workqueue %ld!\n", i);

        struct workqueue_attributes attrs = {
            .capacity = DEFAULT_WORKQUEUE_CAPACITY,
            .max_workers = DEFAULT_MAX_WORKERS,
            .spawn_delay = DEFAULT_SPAWN_DELAY,
            .inactive_check_period.min = DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD,
            .inactive_check_period.max = DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD,
            .flags = WORKQUEUE_FLAG_PERMANENT | WORKQUEUE_FLAG_AUTO_SPAWN |
                     WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS,
        };

        global.workqueues[i] = workqueue_create(&attrs);

        if (!global.workqueues[i])
            k_panic("Failed to spawn permanent workqueue\n");

        uint64_t core_id = i;
        spawn_permanent_thread_on_core(core_id);
    }
}
