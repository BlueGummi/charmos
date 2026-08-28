#include "internal.h"

#include <nightmare/record.h>
#include <sch/sched.h>
#include <thread/thread.h>

#ifdef TEST_NIGHTMARE_ENABLED
void nightmare_publish_stop(enum nightmare_stop reason) {
    enum nightmare_stop observed =
        atomic_load_explicit(&nightmare_runtime.stop, memory_order_acquire);
    bool advanced = false;
    while (observed < reason) {
        if (atomic_compare_exchange_weak_explicit(
                &nightmare_runtime.stop, &observed, reason,
                memory_order_release, memory_order_acquire)) {
            advanced = true;
            break;
        }
    }

    /* Workers and heartbeat may be between polls in timed sleeps, so
     * we need to wake them here so teardown doesn't depend on their timers */

    /* Threads blocked on other stuff don't care because the expected
     * wake source won't match what we give them here */
    if (advanced && observed == NM_RUN) {
        for (size_t i = 0; i < nightmare_runtime.total_worker_count; i++) {
            struct thread *thread = atomic_load_explicit(
                &nightmare_runtime.workers[i].th, memory_order_acquire);
            if (thread)
                scheduler_wake_manual(thread, thread);
        }
        if (nightmare_runtime.heartbeat)
            scheduler_wake_manual(nightmare_runtime.heartbeat,
                                  nightmare_runtime.heartbeat);
    }
}

bool nightmare_must_stop_irq(void) {
    return atomic_load_explicit(&nightmare_runtime.stop,
                                memory_order_acquire) != NM_RUN;
}

bool nightmare_must_stop(void) {
    return nightmare_must_stop_irq();
}

void nightmare_stop_after_finding(void) {
    nightmare_publish_stop(NM_STOP_FINDING);
}

bool nightmare_must_park(void) {
    return atomic_load_explicit(&nightmare_runtime.quiesce_requested,
                                memory_order_acquire);
}

void nightmare_park(struct nightmare_worker *worker) {
    bool was_parked =
        atomic_exchange_explicit(&worker->parked, true, memory_order_acq_rel);
    if (!was_parked)
        atomic_fetch_add_explicit(&nightmare_runtime.parked_count, 1,
                                  memory_order_release);

    while (nightmare_must_park() && !nightmare_must_stop())
        scheduler_yield();

    if (!was_parked) {
        atomic_fetch_sub_explicit(&nightmare_runtime.parked_count, 1,
                                  memory_order_release);
        atomic_store_explicit(&worker->parked, false, memory_order_release);
    }
}

void nightmare_thread_main(void *arg) {
    struct nightmare_worker *worker = arg;
    completion_wait(&nightmare_runtime.start);

    if (!nightmare_must_stop()) {
        if (worker->index < nightmare_runtime.ctx.worker_count) {
            if (nightmare_runtime.ctx.nm && nightmare_runtime.ctx.nm->ops &&
                nightmare_runtime.ctx.nm->ops->worker)
                nightmare_runtime.ctx.nm->ops->worker(&nightmare_runtime.ctx,
                                                      worker);
        } else {
            size_t pidx = worker->index - nightmare_runtime.ctx.worker_count;
            if (pidx < nightmare_runtime.perturber_count &&
                nightmare_runtime.perturbers[pidx] &&
                nightmare_runtime.perturbers[pidx]->thread) {
                nightmare_runtime.perturbers[pidx]->thread(
                    &nightmare_runtime.ctx, worker);
            }
        }
    }

    if (atomic_load_explicit(&worker->parked, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&nightmare_runtime.parked_count, 1,
                                  memory_order_release);
        atomic_store_explicit(&worker->parked, false, memory_order_release);
    }
}

void nightmare_heartbeat_main(void *arg) {
    (void) arg;
    completion_wait(&nightmare_runtime.start);

    time_ms_t next = time_get_ms() + nightmare_runtime.stat_interval_ms;
    do {
        nightmare_liveness_poll();

        time_ms_t now = time_get_ms();
        if (now >= next) {
            nightmare_record_stat(nightmare_progress_sum_irq(),
                                  nightmare_runtime.ctx.worker_count);
            next = now + nightmare_runtime.stat_interval_ms;
        }
        thread_sleep_for_ms(10);
    } while (!nightmare_must_stop());

    nightmare_liveness_poll();

    nightmare_record_stat(nightmare_progress_sum_irq(),
                          nightmare_runtime.ctx.worker_count);
}
#endif
