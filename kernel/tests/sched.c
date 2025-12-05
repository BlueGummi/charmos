#ifdef TEST_SCHED

#include <sch/daemon.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <string.h>
#include <tests.h>

static atomic_bool workqueue_ran = false;
static _Atomic uint32_t workqueue_times = 0;
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

    char *msg = kzalloc(100, ALLOC_PARAMS_DEFAULT);
    TEST_ASSERT(msg);
    snprintf(msg, 100, "Took %d clock cycles to add to event pool %d times",
             total, times);
    ADD_MESSAGE(msg);

    TEST_ASSERT(atomic_load(&workqueue_ran));

    msg = kzalloc(100, ALLOC_PARAMS_DEFAULT);
    snprintf(msg, 100,
             "Event pool ran %d times, tests should've had it run %d times",
             workqueue_times, times);
    ADD_MESSAGE(msg);

    SET_SUCCESS();
}

static void sleepy_entry(void *) {
    thread_sleep_for_ms(9000);
    thread_print(scheduler_get_current_thread());
}

REGISTER_TEST(sched_sleepy_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    thread_spawn("sched_sleepy_test", sleepy_entry, NULL);
    SET_SUCCESS();
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

REGISTER_TEST(workqueue_test_2, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct cpu_mask mask;
    if (!cpu_mask_init(&mask, global.core_count))
        k_panic("OOM\n");

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

    wq = workqueue_create(&attrs, /* fmt = */ NULL);

    for (size_t i = 0; i < WQ_2_THREADS; i++) {
        k_printf("spawning workqueue enqueue threads\n");
        thread_spawn("workqueue_enqueue_thread", enqueue_thread, NULL);
    }

    k_printf("yielding\n");
    thread_apply_cpu_penalty(scheduler_get_current_thread());
    while (atomic_load(&threads_left) > 0) {
        scheduler_yield();
    }

    uint64_t workers = wq->num_workers;

    char *msg = kmalloc(100, ALLOC_PARAMS_DEFAULT);
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

static atomic_bool si_apc_ran = false;
static struct thread *si_t;
static atomic_bool si_ok = false;
static atomic_bool si_started = false;

static void apc_si(struct apc *apc, void *a, void *b) {
    (void) a, (void) b, (void) apc;
    atomic_store(&si_apc_ran, true);
}

static void apc_enqueue_thread(void *) {
    struct apc *apc = apc_create();
    apc_init(apc, apc_si, NULL, NULL);

    while (!atomic_load(&si_started))
        cpu_relax();

    apc_enqueue(si_t, apc, APC_TYPE_KERNEL);
}

static void sleeping_thread(void *) {
    atomic_store(&si_started, true);

    thread_sleep(scheduler_get_current_thread(), THREAD_SLEEP_REASON_MANUAL,
                 THREAD_WAIT_INTERRUPTIBLE, (void *) 4);

    thread_wait_for_wake_match();

    atomic_store(&si_ok, true);
}

static void waking_thread(void *) {
    while (!atomic_load(&si_apc_ran))
        scheduler_yield();

    scheduler_wake(si_t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                   si_t->perceived_prio_class, (void *) 4);
}

REGISTER_TEST(thread_sleep_interruptible_test, SHOULD_NOT_FAIL,
              IS_INTEGRATION_TEST) {
    if (global.core_count < 4) {
        ADD_MESSAGE("too few cores");
        SET_SKIP();
        return;
    }

    si_t = thread_spawn_on_core("si_thread", sleeping_thread, NULL, 1);
    thread_spawn_on_core("si_wake", waking_thread, NULL, 2);
    thread_spawn_on_core("si_apc_e", apc_enqueue_thread, NULL, 3);
    while (!atomic_load(&si_ok))
        scheduler_yield();

    SET_SUCCESS();
}

#endif
