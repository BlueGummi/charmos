#include "internal.h"

_Static_assert(DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD / 4 >
                   DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD,
               "");

static time_t get_inactivity_timeout(struct workqueue *queue) {
    uint32_t num_workers = atomic_load(&queue->num_workers);
    size_t min = queue->attrs.inactive_check_period.min;
    size_t max = queue->attrs.inactive_check_period.max;

    if (num_workers <= (queue->attrs.max_workers / 8))
        return max;

    if (num_workers <= (queue->attrs.max_workers / 4))
        return max / 2;

    if (num_workers <= (queue->attrs.max_workers / 2))
        return max / 4;

    return min;
}

void workqueue_link_thread_and_worker(struct worker *worker,
                                      struct thread *thread) {
    worker->present = true;
    worker->timeout_ran = true;
    worker->thread = thread;
    thread->worker = worker;
}

static bool claim_spawner(struct workqueue *p) {
    return atomic_flag_test_and_set_explicit(&p->spawner_flag_internal,
                                             memory_order_acq_rel) == 0;
}

static void release_spawner(struct workqueue *p) {
    atomic_flag_clear_explicit(&p->spawner_flag_internal, memory_order_release);
}

static void worker_complete_init(struct workqueue *queue, struct worker *w,
                                 struct thread *t) {
    w->thread = t;
    w->present = true;
    w->workqueue = queue;
    w->timeout_ran = true;
    queue->last_spawn_attempt = time_get_ms();

    refcount_init(&w->refcount, 1);

    t->worker = w;

    atomic_fetch_add(&queue->num_workers, 1);
}

bool workqueue_spawn_worker(struct workqueue *queue) {
    if (!claim_spawner(queue))
        return false;

    struct worker *w = kzalloc(sizeof(struct worker));
    if (!w)
        goto fail;

    w->inactivity_check_period = get_inactivity_timeout(queue);

    workqueue_add_worker(queue, w);

    struct thread *t;
    if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS))
        t = worker_create_unmigratable();
    else
        t = worker_create();

    if (!t)
        goto fail;

    worker_complete_init(queue, w, t);

    if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_PERMANENT)) {
        scheduler_enqueue_on_core(t, queue->core);
    } else {
        scheduler_enqueue(t);
    }

    release_spawner(queue);
    return true;

fail:
    if (w)
        kfree(w);

    release_spawner(queue);
    return false;
}

static bool should_spawn_worker(struct workqueue *queue) {
    time_t now = time_get_ms_fast();
    if (now - queue->last_spawn_attempt <= queue->attrs.spawn_delay)
        return false;

    bool no_idle = atomic_load(&queue->idle_workers) == 0;
    bool work_pending = !workqueue_empty(queue);

    bool under_limit =
        atomic_load(&queue->num_workers) < queue->attrs.max_workers;

    /* Permanent workqueues are per-core and spawning
     * extra threads on them doesn't help */
    bool non_permanent = !WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_PERMANENT);

    return no_idle && work_pending && under_limit && non_permanent;
}

bool workqueue_try_spawn_worker(struct workqueue *queue) {
    if (!should_spawn_worker(queue))
        return false;

    if (in_interrupt()) {
        workqueue_set_needs_spawn(queue, true);
        return true;
    }

    return workqueue_spawn_worker(queue);
}

struct thread *worker_create(void) {
    uint64_t stack_size = PAGE_SIZE;
    return thread_create_custom_stack(worker_main, stack_size);
}

struct thread *worker_create_unmigratable() {
    uint64_t stack_size = PAGE_SIZE;
    struct thread *t = thread_create_custom_stack(worker_main, stack_size);
    if (!t)
        return NULL;

    t->flags = THREAD_FLAGS_NO_STEAL;
    return t;
}
