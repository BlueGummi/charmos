#include "internal.h"
#include <charmos.h>
#include <mem/alloc.h>

static void spawn_permanent_thread_on_core(uint64_t core) {
    struct workqueue *queue = workqueues[core];
    queue->core = core;

    struct thread *thread = worker_create_unmigratable();
    if (!thread) {
        k_panic("Failed to spawn permanent worker thread on core %llu\n", core);
    }

    struct worker_thread *worker = &queue->workers[0];
    worker->is_permanent = true;
    worker->inactivity_check_period = MINUTES_TO_MS(5);
    workqueue_link_thread_and_worker(worker, thread);
    scheduler_enqueue_on_core(thread, core);
    workqueue_update_queue_after_spawn(queue);
}

void workqueue_init(void) {
    num_workqueues = global.core_count;
    workqueues = kzalloc(sizeof(struct workqueue *) * num_workqueues);

    if (!workqueues)
        k_panic("Failed to allocate space for workqueues!\n");

    for (int64_t i = 0; i < num_workqueues; ++i) {

        workqueues[i] = kzalloc(sizeof(struct workqueue));
        if (!workqueues[i])
            k_panic("Failed to allocate space for workqueue %ld!\n", i);

        spinlock_init(&workqueues[i]->lock);
        workqueues[i]->capacity = DEFAULT_WORKQUEUE_CAPACITY;
        workqueues[i]->max_workers = DEFAULT_MAX_WORKERS;
        workqueues[i]->spawn_delay = DEFAULT_SPAWN_DELAY;

        workqueues[i]->interactivity_check_period.min =
            DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD;

        workqueues[i]->interactivity_check_period.max =
            DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD;

        workqueues[i]->tasks =
            kzalloc(sizeof(struct worker_task) * workqueues[i]->capacity);

        workqueues[i]->workers =
            kzalloc(sizeof(struct worker_thread) * workqueues[i]->max_workers);

        for (uint64_t j = 0; j < DEFAULT_WORKQUEUE_CAPACITY; ++j)
            atomic_store_explicit(&workqueues[i]->tasks[j].seq, j,
                                  memory_order_relaxed);

        uint64_t core_id = i;
        spawn_permanent_thread_on_core(core_id);
    }
}
