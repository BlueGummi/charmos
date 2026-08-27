#include "internal.h"

#include <nightmare/record.h>
#include <sch/sched.h>
#include <thread/thread.h>

#ifdef TEST_NIGHTMARE_ENABLED
void nightmare_publish_stop(enum nightmare_stop reason) {
    enum nightmare_stop observed =
        atomic_load_explicit(&nightmare_runtime.stop, memory_order_acquire);
    while (observed < reason && !atomic_compare_exchange_weak_explicit(
                                    &nightmare_runtime.stop, &observed, reason,
                                    memory_order_release, memory_order_acquire))
        ;
}

bool nightmare_must_stop_irq(void) {
    return atomic_load_explicit(&nightmare_runtime.stop,
                                memory_order_acquire) != NM_RUN;
}

bool nightmare_must_stop(void) {
    return nightmare_must_stop_irq();
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

uint64_t nightmare_rand(struct nightmare_rng *rng) {
    uint64_t z = (rng->state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
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
        time_ms_t now = time_get_ms();
        if (now >= next) {
            nightmare_record_stat(nightmare_progress_sum_irq(),
                                  nightmare_runtime.ctx.worker_count);
            next = now + nightmare_runtime.stat_interval_ms;
        }
        thread_sleep_for_ms(10);
    } while (!nightmare_must_stop());

    nightmare_record_stat(nightmare_progress_sum_irq(),
                          nightmare_runtime.ctx.worker_count);
}
#endif
