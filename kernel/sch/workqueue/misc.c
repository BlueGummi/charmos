#include "internal.h"

struct workqueue *workqueue_least_loaded_queue_except(int64_t except_core_num) {
    uint64_t minimum_load = UINT64_MAX;

    if (except_core_num == -1) {
        /* don't avoid any core */
    }

    /* There will always be a 'core 0 thread' */
    struct workqueue *least_loaded = global.workqueues[0];
    for (int64_t i = 0; i < (int64_t) global.core_count; i++) {
        if (WORKQUEUE_NUM_WORKS(global.workqueues[i]) < minimum_load &&
            i != except_core_num) {
            minimum_load = WORKQUEUE_NUM_WORKS(global.workqueues[i]);
            least_loaded = global.workqueues[i];
        }
    }

    return least_loaded;
}

struct workqueue *workqueue_get_least_loaded(void) {
    return workqueue_least_loaded_queue_except(-1);
}

struct workqueue *workqueue_get_least_loaded_remote(void) {
    return workqueue_least_loaded_queue_except(smp_core_id());
}
