#include "internal.h"

_Static_assert(WORKQUEUE_DEFAULT_MAX_IDLE_CHECK / 4 >
                   WORKQUEUE_DEFAULT_MIN_IDLE_CHECK,
               "");

static time_t get_inactivity_timeout(struct workqueue *queue) {
    uint32_t num_workers = atomic_load(&queue->num_workers);
    size_t min = queue->attrs.idle_check.min;
    size_t max = queue->attrs.idle_check.max;

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

    thread->private = worker;
}

static bool claim_spawner(struct workqueue *p) {
    return atomic_flag_test_and_set_explicit(&p->spawner_flag_internal,
                                             memory_order_acq_rel) == 0;
}

static void release_spawner(struct workqueue *p) {
    atomic_flag_clear_explicit(&p->spawner_flag_internal, memory_order_release);
}

static void worker_init(struct workqueue *queue, struct worker *w,
                        struct thread *t) {
    INIT_LIST_HEAD(&w->list_node);
    w->thread = t;
    w->present = true;
    w->workqueue = queue;
    w->timeout_ran = true;
    queue->last_spawn_attempt = time_get_ms();

    t->private = w;

    atomic_fetch_add(&queue->num_workers, 1);
}

static struct thread *workqueue_worker_thread_create(struct workqueue *queue) {
    if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS))
        return worker_create_unmigratable(queue->attrs.worker_cpu_mask);
    else
        return worker_create(queue->attrs.worker_cpu_mask);
}

static void workqueue_enqueue_thread(struct workqueue *queue,
                                     struct thread *t) {
    if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_PERMANENT))
        scheduler_enqueue_on_core(t, queue->core);
    else
        scheduler_enqueue(t);
}

struct worker *workqueue_worker_create(struct workqueue *queue) {
    if (queue->attrs.flags & WORKQUEUE_FLAG_STATIC_WORKERS) {
        enum irql irql = workqueue_worker_array_lock(queue);
        struct worker *ret = NULL;
        for (size_t i = 0; i < queue->attrs.max_workers; i++) {
            if (queue->worker_array[i].thread == NULL) {
                ret = &queue->worker_array[i];
                goto out;
            }
        }

    out:
        workqueue_worker_array_unlock(queue, irql);
        return ret;
    } else {
        return kzalloc(sizeof(struct worker));
    }
}

static void workqueue_init_new_worker(struct workqueue *queue, struct worker *w,
                                      struct thread *t) {
    w->inactivity_check_period = get_inactivity_timeout(queue);

    workqueue_add_worker(queue, w);
    worker_init(queue, w, t);
    workqueue_enqueue_thread(queue, t);
}

enum thread_request_decision workqueue_request_callback(struct thread *t,
                                                        void *data) {
    struct workqueue *queue = data;
    struct worker *worker = workqueue_worker_create(queue);
    if (!worker) {
        workqueue_put(queue);
        return THREAD_REQUEST_DECISION_DESTROY; /* whatever... we can't
                                                 * make this thread anyways */
    }

    /* splendid, let's enqueue it */
    workqueue_init_new_worker(queue, worker, t);
    workqueue_put(queue);
    return THREAD_REQUEST_DECISION_KEEP;
}

void workqueue_spawn_via_request(struct workqueue *queue) {
    kassert(queue->attrs.flags & WORKQUEUE_FLAG_SPAWN_VIA_REQUEST);
    if (!claim_spawner(queue))
        return;

    /* We just submit our request, and in the callback see if we
     * still need a new thread */
    if (THREAD_REQUEST_STATE(queue->request) == THREAD_REQUEST_PENDING)
        return;

    workqueue_get(queue);
    thread_request_enqueue(queue->request);

    release_spawner(queue);
}

/* This is only for non-request based worker thread spawning */
bool workqueue_spawn_worker_internal(struct workqueue *queue) {
    kassert(!(queue->attrs.flags & WORKQUEUE_FLAG_SPAWN_VIA_REQUEST));
    if (!claim_spawner(queue))
        return false;

    struct worker *w = workqueue_worker_create(queue);
    if (!w)
        goto fail;

    struct thread *t = workqueue_worker_thread_create(queue);
    if (!t)
        goto fail;

    workqueue_init_new_worker(queue, w, t);

    release_spawner(queue);
    return true;

fail:
    if (w && !(queue->attrs.flags & WORKQUEUE_FLAG_STATIC_WORKERS))
        kfree(w);

    release_spawner(queue);
    return false;
}

bool workqueue_should_spawn_worker(struct workqueue *queue) {
    time_t now = time_get_ms();
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
    if (!workqueue_should_spawn_worker(queue))
        return false;

    if (irq_in_interrupt()) {
        workqueue_set_needs_spawn(queue, true);
        return true;
    }

    return workqueue_spawn_worker_internal(queue);
}

struct thread *worker_create(struct cpu_mask mask) {
    uint64_t stack_size = PAGE_SIZE;
    struct thread *ret =
        thread_create_custom_stack("workqueue_worker", worker_main, stack_size);
    if (!ret)
        return NULL;

    ret->allowed_cpus = mask;
    return ret;
}

struct thread *worker_create_unmigratable(struct cpu_mask mask) {
    uint64_t stack_size = PAGE_SIZE;
    struct thread *t =
        thread_create_custom_stack("workqueue_worker", worker_main, stack_size);
    if (!t)
        return NULL;

    thread_set_flags(t, THREAD_FLAGS_NO_STEAL);
    t->allowed_cpus = mask;
    return t;
}
