#include <console/printf.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* for work_steal_victim_min_diff */
static inline uint8_t ilog2(uint64_t x) {
    uint8_t r = 0;
    while (x >>= 1)
        r++;
    return r;
}

uint64_t scheduler_compute_steal_threshold(uint64_t threads) {
    if (global.core_count == 0) {
        k_panic("Why do you have no cores on your machine?\n");
        return 150; // safety fallback
    }

    uint64_t threads_per_core = threads / global.core_count;

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
    if (global.core_count == 0) {
        k_panic("Why are there no cores on your machine?\n");
    }

    int64_t val = atomic_load(&scheduler_data.total_threads);
    int64_t avg_core_threads = val / global.core_count;

    // steal if this core's load is less than WORK_STEAL_THRESHOLD% of average
    uint64_t threshold_load =
        ((avg_core_threads * WORK_STEAL_THRESHOLD) / 100ULL) ?: 1;

    return (sched->thread_count < threshold_load);
}
