#include "thread/workqueue/tests/test_internal.h"

TEST_GROUP_DEFINE(workqueue, .intensity_desc = {
                                 .curve = SCALE_PIECEWISE_LOG,
                                 .unit = "items",
                             });

#define WQ_2_THREADS 2

static atomic_uint32_t times_2 = 0;
static size_t wq_2_items_per_thread = 512;

static void wq_test_2(void *a, void *b) {
    cc_unused(a, b);

    atomic_inc(&times_2);
    for (uint64_t i = 0; i < 500; i++)
        cpu_pause();
}

static struct workqueue *wq = NULL;
static atomic_uint32_t threads_left = WQ_2_THREADS;

static void enqueue_thread(void *arg) {
    cc_unused(arg);
    struct work works[wq_2_items_per_thread];

    for (size_t i = 0; i < wq_2_items_per_thread; i++) {
        for (uint64_t j = 0; j < 500; j++)
            cpu_pause();

        work_init(&works[i], wq_test_2, WORK_ARGS(NULL, wq));
        workqueue_enqueue(wq, &works[i]);
        scheduler_yield();
    }

    for (size_t i = 0; i < wq_2_items_per_thread; i++)
        while (work_active(&works[i]))
            cpu_pause();

    atomic_dec(&threads_left);
}

TEST_DEFINE_UNIT(workqueue, mt_enqueue_scaling,
                 TEST_INTENSITY(512, 4096, 32768)) {
    size_t total_items = ctx->intensity_val ? ctx->intensity_val : 4096;
    wq_2_items_per_thread = total_items / WQ_2_THREADS;
    atomic_store(&times_2, 0);
    atomic_store(&threads_left, WQ_2_THREADS);

    struct cpu_mask mask;
    cpu_mask_init(&mask, global.core_count);

    cpu_mask_set_all(&mask);

    struct workqueue_attributes attrs = {
        .flags = WORKQUEUE_FLAG_AUTO_SPAWN | WORKQUEUE_FLAG_ON_DEMAND,
        .spawn_delay = 1,
        .idle_check.max = 10000,
        .idle_check.min = 2000,
        .min_workers = 2,
        .max_workers = 64,
        .worker_cpu_mask = mask,
    };

    wq = workqueue_create(NULL, &attrs);

    struct thread *enqueuers[WQ_2_THREADS];
    for (size_t i = 0; i < WQ_2_THREADS; i++) {
        test_info("spawning workqueue enqueue threads");

        /* Big stack */
        enqueuers[i] = thread_spawn("workqueue_enqueue_thread", enqueue_thread,
                                    .stack_pages = 32, .joinable = true);
    }

    test_info("waiting for enqueue threads");
    for (size_t i = 0; i < WQ_2_THREADS; i++) {
        if (enqueuers[i])
            thread_join(enqueuers[i]);
    }

    TEST_ASSERT_EQ(atomic_load(&threads_left), 0);

    uint64_t workers = atomic_load_rlx(&wq->num_workers);

    char *msg = valloc(100, ALLOC_ZERO);
    if (msg) {
        snprintf(msg, 100, "There are %lu workers", workers);
        test_info("%s", msg);
        vfree(msg);
    }

    test_info("destroy");
    workqueue_destroy(wq);
    return TEST_SUCCESS;
}
