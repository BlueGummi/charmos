#include "internal.h"

_Static_assert(DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD / 4 >
                   DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD,
               "");

static time_t get_inactivity_timeout(struct workqueue *queue) {
    uint32_t num_workers = atomic_load(&queue->num_workers);
    size_t min = queue->interactivity_check_period.min;
    size_t max = queue->interactivity_check_period.max;

    if (num_workers <= (queue->max_workers / 8))
        return max;

    if (num_workers <= (queue->max_workers / 4))
        return max / 2;

    if (num_workers <= (queue->max_workers / 2))
        return max / 4;

    return min;
}

int workqueue_reserve_slot(struct workqueue *queue) {
    for (uint64_t i = 1; i < queue->max_workers; ++i) {
        uint64_t bit = 1ull << i;
        uint64_t old =
            atomic_load_explicit(&queue->worker_bitmap, memory_order_relaxed);

        if (old & bit)
            continue; /* already used */
        if (atomic_compare_exchange_weak_explicit(
                &queue->worker_bitmap, &old, old | bit, memory_order_acq_rel,
                memory_order_relaxed)) {
            return (int) i;
        }
        /* retry - someone else raced */
    }

    k_panic("Unreachable, all worker slots in use\n");
    return -1;
}

void workqueue_unreserve_slot(struct workqueue *queue, int idx) {
    uint64_t bit = ~(1ull << idx);
    atomic_fetch_and_explicit(&queue->worker_bitmap, bit, memory_order_acq_rel);
}

void workqueue_link_thread_and_worker(struct worker_thread *worker,
                                      struct thread *thread) {
    worker->present = true;
    worker->timeout_ran = true;
    worker->thread = thread;
    thread->worker = worker;
}

void workqueue_update_queue_after_spawn(struct workqueue *queue) {
    atomic_fetch_add(&queue->num_workers, 1);
    queue->last_spawn_attempt = time_get_ms();
}

static bool claim_spawner(struct workqueue *p) {
    return atomic_flag_test_and_set_explicit(&p->spawner_flag,
                                             memory_order_acq_rel) == 0;
}

static void release_spawner(struct workqueue *p) {
    atomic_flag_clear_explicit(&p->spawner_flag, memory_order_release);
}

bool workqueue_spawn_worker(struct workqueue *queue) {
    if (!claim_spawner(queue))
        return false;

    int slot = workqueue_reserve_slot(queue);
    if (slot < 0) {
        release_spawner(queue);
        return false;
    }

    struct worker_thread *w = &queue->workers[slot];
    w->inactivity_check_period = get_inactivity_timeout(queue);

    struct thread *t = worker_create();
    if (!t) {
        workqueue_unreserve_slot(queue, slot);
        release_spawner(queue);
        return false;
    }

    w->thread = t;
    w->present = true;
    w->timeout_ran = true;
    t->worker = w;

    atomic_fetch_add(&queue->num_workers, 1);
    atomic_fetch_add(&queue->total_spawned, 1);
    queue->last_spawn_attempt = time_get_ms(); /* This is a slowpath so we
                                                * can safely run this
                                                * more costly MMIO op */

    scheduler_enqueue_on_core(t, queue->core);

    release_spawner(queue);

    return true;
}

static bool should_spawn_worker(struct workqueue *queue) {
    time_t now = time_get_ms_fast();
    if (now - queue->last_spawn_attempt <= queue->spawn_delay)
        return false;

    bool no_idle = atomic_load(&queue->idle_workers) == 0;
    bool work_pending = atomic_load(&queue->head) != atomic_load(&queue->tail);

    bool under_limit = atomic_load(&queue->num_workers) < queue->max_workers;
    bool not_spawning = !atomic_flag_test_and_set(&queue->spawner_flag);

    return no_idle && work_pending && under_limit && not_spawning;
}

bool workqueue_try_spawn_worker(struct workqueue *queue) {
    if (!should_spawn_worker(queue))
        return false;

    if (in_interrupt()) {
        atomic_store(&queue->spawn_pending, true);
        return true;
    }

    return workqueue_spawn_worker(queue);
}
