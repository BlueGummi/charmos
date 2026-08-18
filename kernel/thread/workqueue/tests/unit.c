#include "test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(workqueue);

static atomic_bool workqueue_ran = false;
static _Atomic uint32_t workqueue_times = 0;
static void workqueue_fn(void *arg, void *unused) {
    (void) arg, (void) unused;
    atomic_store(&workqueue_ran, true);
    atomic_fetch_add(&workqueue_times, 1);
}

TEST_DECLARE_UNIT(workqueue_test, .group = TEST_GROUP(workqueue)) {
    uint64_t tsc = rdtsc();
    uint64_t times = 256;

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
    snprintf(msg, 100, "Took %d clock cycles to add to event pool %d times",
             total, times);
    test_info(msg);

    TEST_ASSERT(atomic_load(&workqueue_ran));

    msg = kmalloc(100, ALLOC_FLAGS_ZERO);
    snprintf(msg, 100,
             "Event pool ran %d times, tests should've had it run %d times",
             workqueue_times, times);
    test_info(msg);

    return TEST_SUCCESS;
}

#define WQ_2_TIMES 4096
#define WQ_2_THREADS 2

static _Atomic uint32_t times_2 = 0;

static void wq_test_2(void *a, void *b) {
    (void) a, (void) b;
    atomic_fetch_add(&times_2, 1);
    for (uint64_t i = 0; i < 500; i++)
        cpu_relax();
}

static struct workqueue *wq = NULL;
static _Atomic uint32_t threads_left = WQ_2_THREADS;

static void enqueue_thread(void *) {
    for (size_t i = 0; i < WQ_2_TIMES / WQ_2_THREADS; i++) {
        for (uint64_t i = 0; i < 500; i++)
            cpu_relax();

        workqueue_enqueue_oneshot(wq, wq_test_2, WORK_ARGS(NULL, wq));
        scheduler_yield();
    }
    atomic_fetch_sub(&threads_left, 1);
}

TEST_DECLARE_UNIT(workqueue_test_2, .group = TEST_GROUP(workqueue)) {
    struct cpu_mask mask;
    alloc_or_die(cpu_mask_init(&mask, global.core_count));

    cpu_mask_set_all(&mask);

    struct workqueue_attributes attrs = {
        .capacity = WQ_2_TIMES,
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

    char *msg = kmalloc(100);
    snprintf(msg, 100, "There are %d workers", workers);
    test_info(msg);

    test_info("destroy");
    workqueue_destroy(wq);
    return TEST_SUCCESS;
}
#endif

#ifdef TEST_TIMER_DEFER
TEST_GROUP_DECLARE(defer);

static atomic_bool defer_worked = false;
static uint64_t enqueue_ms;
static uint64_t finish_ms;
static char msg[100] = {0};
static struct delayed_work test_dwork;

static void defer_func(void *boo, void *unused) {
    (void) boo, (void) unused;
    finish_ms = time_get_ms();

    snprintf(msg, sizeof(msg), "Start ms was %lu, end ms was %lu, took %lu ms",
             enqueue_ms, finish_ms, finish_ms - enqueue_ms);

    test_info("Delayed work complete");
    test_info(msg);
    defer_worked = true;
}

TEST_DECLARE_UNIT(defer_test, .group = TEST_GROUP(defer)) {
    atomic_store(&defer_worked, false);
    delayed_work_init(&test_dwork, defer_func, WORK_ARGS(NULL, NULL));
    enqueue_ms = time_get_ms();
    delayed_work_schedule(&test_dwork, 5);

    time_ms_t deadline = time_get_ms() + 500;
    while (!defer_worked && time_get_ms() < deadline)
        scheduler_yield();

    TEST_ASSERT(defer_worked);
    return TEST_SUCCESS;
}
#endif
