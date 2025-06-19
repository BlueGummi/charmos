#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <spin_lock.h>
#include <stdatomic.h>

/* for work_steal_victim_min_diff */
static inline uint8_t ilog2(uint64_t x) {
    uint8_t r = 0;
    while (x >>= 1)
        r++;
    return r;
}

uint64_t compute_steal_threshold(uint64_t total_threads, uint64_t core_count) {
    if (core_count == 0) {
        k_panic("Why do you have no cores on your machine?\n");
        return 150; // safety fallback
    }

    uint64_t threads_per_core = total_threads / core_count;

    // very low thread/core ratio, be conservative
    if (threads_per_core <= 1)
        return 150;

    // very high ratio, allow aggressive stealing
    if (threads_per_core >= 64)
        return 110;

    uint8_t log = ilog2(threads_per_core);
    return 150 - (log * 5);
}

bool scheduler_can_steal_work(struct scheduler *sched) {
    if (c_count == 0) {
        k_panic("Why are there no cores on your machine?\n");
    }

    uint64_t val = atomic_load(&global_load);
    uint64_t avg_core_load = val / c_count;

    // steal if this core's load is less than WORK_STEAL_THRESHOLD% of average
    uint64_t threshold_load =
        ((avg_core_load * WORK_STEAL_THRESHOLD) / 100ULL) ?: 1;

    return (sched->load < threshold_load);
}

uint64_t scheduler_compute_load(struct scheduler *sched, uint64_t alpha_scaled,
                                uint64_t beta_scaled) {
    if (!sched)
        return 0;

    uint64_t ready_count = 0;
    uint64_t weighted_sum = 0;

    for (int level = 0; level < MLFQ_LEVELS; level++) {
        struct thread_queue *q = &sched->queues[level];
        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *current = start;

        do {
            if (current->state == READY) {
                ready_count++;
                // Weight lower levels more heavily
                weighted_sum += (MLFQ_LEVELS - level);
            }
            current = current->next;
        } while (current != start);
    }

    if (ready_count == 0)
        return 0;

    // floating point math is bad so we scale it
    uint64_t load_scaled =
        ready_count *
        (alpha_scaled + (beta_scaled * weighted_sum) / ready_count);

    return load_scaled;
}
