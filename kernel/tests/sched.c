#include <sch/daemon.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <string.h>
#include <tests.h>

static volatile atomic_bool ran = false;

static void testfn(void) {
    atomic_store(&ran, true);
    ADD_MESSAGE("testfn ran");
}

REGISTER_TEST(sched_reaper_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint64_t reaped_threads_at_start = reaper_get_reaped_thread_count();
    thread_spawn(testfn);
    enable_interrupts();

    scheduler_yield();

    while (!atomic_load(&ran))
        ;

    uint64_t timeout = 5000;
    while (reaper_get_reaped_thread_count() <= reaped_threads_at_start &&
           timeout--)
        sleep_us(10);

    if (!timeout) {
        SET_FAIL();
        return;
    }

    SET_SUCCESS();
}

static atomic_bool workqueue_ran = false;
static atomic_uint workqueue_times = 0;
static void workqueue_fn(void *arg, void *unused) {
    (void) arg, (void) unused;
    atomic_store(&workqueue_ran, true);
    atomic_fetch_add(&workqueue_times, 1);
}

REGISTER_TEST(workqueue_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint64_t tsc = rdtsc();
    uint64_t times = 256;

    for (uint64_t i = 0; i < times; i++)
        workqueue_add_fast_oneshot(workqueue_fn, WORK_ARGS(NULL, NULL));

    uint64_t total = rdtsc() - tsc;
    sleep_ms(50);

    while (!atomic_load(&workqueue_ran))
        cpu_relax();

    char *msg = kzalloc(100);
    TEST_ASSERT(msg);
    snprintf(msg, 100, "Took %d clock cycles to add to event pool %d times",
             total, times);
    ADD_MESSAGE(msg);

    TEST_ASSERT(atomic_load(&workqueue_ran));

    msg = kzalloc(100);
    snprintf(msg, 100,
             "Event pool ran %d times, tests should've had it run %d times",
             workqueue_times, times);
    ADD_MESSAGE(msg);

    SET_SUCCESS();
}

static atomic_bool rt_thread_fail = false;
static struct thread *rt = NULL;

static void rt_thread(void) {
    uint64_t spins = 50;
    struct thread *me = scheduler_get_current_thread();
    if (me != rt) {
        k_printf("Different thread\n");
        goto fail;
    }

    for (uint64_t i = 0; i < spins; i++) {
        /* This sleep function just
         * busy wait-polls the timer */
        sleep_ms(1);
    }

    return;

fail:
    atomic_store(&rt_thread_fail, true);
    return;
}

REGISTER_TEST(rt_thread_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct thread *thread = thread_create(rt_thread);
    rt = thread;
    thread->base_priority = THREAD_PRIO_CLASS_URGENT;
    thread->perceived_priority = THREAD_PRIO_CLASS_URGENT;

    scheduler_enqueue_on_core(thread, smp_core_id());
    scheduler_yield();
    TEST_ASSERT(!atomic_load(&rt_thread_fail));

    SET_SUCCESS();
}

static atomic_uint ran_count = 0;
static void sched_spawn_entry(void) {
    atomic_fetch_add(&ran_count, 1);
}

REGISTER_TEST(sched_spawn_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    enable_interrupts();
    uint32_t run_times = 50;

    for (uint32_t i = 0; i < run_times; i++)
        thread_spawn(sched_spawn_entry);

    while (run_times != ran_count)
        scheduler_yield();

    TEST_ASSERT(run_times == ran_count);

    SET_SUCCESS();
}

static void sleepy_entry(void) {
    thread_sleep_for_ms(9000);
    thread_print(scheduler_get_current_thread());
}

REGISTER_TEST(sched_sleepy_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    thread_spawn(sleepy_entry);
    SET_SUCCESS();
}

#define WQ_2_TIMES 4096
#define WQ_2_THREADS 2

static atomic_uint times_2 = 0;

static void wq_test_2(void *a, void *b) {
    (void) a, (void) b;
    atomic_fetch_add(&times_2, 1);
    for (uint64_t i = 0; i < 500; i++)
        cpu_relax();
}

static struct workqueue *wq = NULL;
static atomic_uint threads_left = WQ_2_THREADS;

static void enqueue_thread(void) {
    for (size_t i = 0; i < WQ_2_TIMES / WQ_2_THREADS; i++) {
        for (uint64_t i = 0; i < 500; i++)
            cpu_relax();

        while (workqueue_enqueue_oneshot(wq, wq_test_2, WORK_ARGS(NULL, wq)) ==
               WORKQUEUE_ERROR_FULL)
            scheduler_yield();
    }
    atomic_fetch_sub(&threads_left, 1);
}

REGISTER_TEST(workqueue_test_2, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct workqueue_attributes attrs = {
        .capacity = WQ_2_TIMES,
        .flags = WORKQUEUE_FLAG_AUTO_SPAWN | WORKQUEUE_FLAG_ON_DEMAND,
        .spawn_delay = 1,
        .inactive_check_period.max = 10000,
        .inactive_check_period.min = 2000,
        .min_workers = 2,
        .max_workers = 64,
    };

    wq = workqueue_create(&attrs, /* fmt = */ NULL);

    for (size_t i = 0; i < WQ_2_THREADS; i++) {
        k_printf("spawning workqueue enqueue threads\n");
        thread_spawn(enqueue_thread);
    }

    k_printf("yielding\n");
    thread_apply_cpu_penalty(scheduler_get_current_thread());
    while (atomic_load(&threads_left) > 0) {
        scheduler_yield();
    }

    uint64_t workers = wq->num_workers;

    char *msg = kmalloc(100);
    snprintf(msg, 100, "There are %d workers", workers);
    ADD_MESSAGE(msg);

    k_printf("destroy\n");
    workqueue_destroy(wq);
    SET_SUCCESS();
}

static atomic_bool daemon_work_run = false;
static enum daemon_thread_command daemon_work(struct daemon_work *work,
                                              struct daemon_thread *thread,
                                              void *a, void *b) {
    (void) work, (void) a, (void) b;
    atomic_store(&daemon_work_run, true);
    return DAEMON_THREAD_COMMAND_SLEEP;
}

static struct daemon_work dwork =
    DAEMON_WORK_FROM(daemon_work, WORK_ARGS(NULL, NULL));

REGISTER_TEST(daemon_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct daemon_attributes attrs = {
        .max_timesharing_threads = 67,
        .flags = DAEMON_FLAG_AUTO_SPAWN | DAEMON_FLAG_HAS_NAME,
    };

    struct daemon *daemon = daemon_create(
        /* attrs = */ &attrs,
        /* timesharing_work = */ &dwork,
        /* background_work = */ NULL,
        /* wq_attrs = */ NULL,
        /* fmt = */ "daemon_test");

    kassert(daemon);

    daemon_wake_timesharing_worker(daemon);
    while (!atomic_load(&daemon_work_run))
        scheduler_yield();

    daemon_destroy(daemon);
    SET_SUCCESS();
}
