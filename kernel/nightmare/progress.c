#include <nightmare/nightmare.h>
#include <smp/percpu.h>

#ifdef TEST_NIGHTMARE_ENABLED
PERCPU_DECLARE(nightmare_progress_store, struct nightmare_progress_counter,
               NULL);
PERCPU_EXPORT_AS(nightmare_progress, nightmare_progress_store);

uint64_t nightmare_progress_sum_irq(void) {
    uint64_t sum = 0;
    struct nightmare_progress_counter *counter;
    percpu_for_each(nightmare_progress, counter) sum +=
        atomic_load_explicit(&counter->count, memory_order_relaxed);
    return sum;
}
#endif
