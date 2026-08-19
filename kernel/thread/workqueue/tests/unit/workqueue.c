#include "../test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(workqueue, .intensity_desc = {
                                  .curve = TEST_SCALE_PIECEWISE_LOG,
                                  .unit = "items",
                              });

static atomic_bool workqueue_ran = false;
static _Atomic uint32_t workqueue_times = 0;
static void workqueue_fn(void *arg, void *unused) {
    (void) arg, (void) unused;
    atomic_store(&workqueue_ran, true);
    atomic_fetch_add(&workqueue_times, 1);
}

TEST_DECLARE_UNIT(workqueue_test, .group = TEST_GROUP(workqueue),
                  TEST_INTENSITY(32, 256, 4096)) {
    atomic_store(&workqueue_ran, false);
    atomic_store(&workqueue_times, 0);

    uint64_t tsc = rdtsc();
    uint64_t times = ctx->intensity_val ? ctx->intensity_val : 256;

    for (uint64_t i = 0; i < times; i++) {
        enum workqueue_error err =
            workqueue_add_fast_oneshot(workqueue_fn, WORK_ARGS(NULL, NULL));
        (void) err;
    }

    uint64_t total = rdtsc() - tsc;
    sleep_spin_ms(50);

    while (!atomic_load(&workqueue_ran))
        cpu_relax();

    char *msg = kmalloc(100, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(msg);
    snprintf(msg, 100, "Took %lu clock cycles to add to event pool %lu times",
             total, times);
    test_info(msg);
    kfree(msg);

    TEST_ASSERT(atomic_load(&workqueue_ran));

    msg = kmalloc(100, ALLOC_FLAGS_ZERO);
    TEST_ASSERT(msg);
    snprintf(msg, 100,
             "Event pool ran %u times, tests should've had it run %lu times",
             atomic_load(&workqueue_times), times);
    test_info(msg);
    kfree(msg);

    return TEST_SUCCESS;
}

#define WQ_2_THREADS 2

static _Atomic uint32_t times_2 = 0;
static size_t wq_2_items_per_thread = 2048;

static void wq_test_2(void *a, void *b) {
    (void) a, (void) b;
    atomic_fetch_add(&times_2, 1);
    for (uint64_t i = 0; i < 500; i++)
        cpu_relax();
}

static struct workqueue *wq = NULL;
static _Atomic uint32_t threads_left = WQ_2_THREADS;

static void enqueue_thread(void *) {
    for (size_t i = 0; i < wq_2_items_per_thread; i++) {
        for (uint64_t j = 0; j < 500; j++)
            cpu_relax();

        workqueue_enqueue_oneshot(wq, wq_test_2, WORK_ARGS(NULL, wq));
        scheduler_yield();
    }
    atomic_fetch_sub(&threads_left, 1);
}

TEST_DECLARE_UNIT(workqueue_test_2, .group = TEST_GROUP(workqueue),
                  TEST_INTENSITY(512, 4096, 32768)) {
    size_t total_items = ctx->intensity_val ? ctx->intensity_val : 4096;
    wq_2_items_per_thread = total_items / WQ_2_THREADS;
    atomic_store(&times_2, 0);
    atomic_store(&threads_left, WQ_2_THREADS);

    struct cpu_mask mask;
    alloc_or_die(cpu_mask_init(&mask, global.core_count));

    cpu_mask_set_all(&mask);

    struct workqueue_attributes attrs = {
        .capacity = total_items,
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
        enqueuers[i] = thread_spawn_joinable("workqueue_enqueue_thread",
                                             enqueue_thread, NULL);
    }

    test_info("waiting for enqueue threads");
    for (size_t i = 0; i < WQ_2_THREADS; i++) {
        if (enqueuers[i])
            thread_join(enqueuers[i]);
    }

    TEST_ASSERT(atomic_load(&threads_left) == 0);

    uint64_t workers = wq->num_workers;

    char *msg = kmalloc(100, ALLOC_FLAGS_ZERO);
    if (msg) {
        snprintf(msg, 100, "There are %lu workers", workers);
        test_info(msg);
        kfree(msg);
    }

    test_info("destroy");
    workqueue_destroy(wq);
    return TEST_SUCCESS;
}
#endif
