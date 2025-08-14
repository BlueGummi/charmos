#include <profiling.h>

struct scheduler_stats {
    atomic_uint_fast64_t steals;
};

#ifdef PROFILING_SCHED
static struct scheduler_stats sched_stats = {0};

static inline void sched_profiling_record_steal(void) {
    atomic_fetch_add(&sched_stats.steals, 1);
}
#else
static inline void sched_profiling_record_steal(void) { /* Nothing */ }
#endif
