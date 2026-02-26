#ifdef TEST_SCHED

#include <sch/sched.h>
#include <sleep.h>
#include <string.h>
#include <tests.h>
#include <thread/apc.h>
#include <thread/daemon.h>
#include <thread/defer.h>
#include <thread/reaper.h>
#include <thread/thread.h>

static atomic_bool workqueue_ran = false;
static _Atomic uint32_t workqueue_times = 0;
static void workqueue_fn(void *arg, void *unused) {
    (void) arg, (void) unused;
    atomic_store(&workqueue_ran, true);
    atomic_fetch_add(&workqueue_times, 1);
}

TEST_REGISTER(workqueue_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
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
    thread_print(thread_get_current());
}

TEST_REGISTER(sched_sleepy_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
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

TEST_REGISTER(workqueue_test_2, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct cpu_mask mask;
    if (!cpu_mask_init(&mask, global.core_count))
        panic("OOM\n");

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

    for (size_t i = 0; i < WQ_2_THREADS; i++) {
        printf("spawning workqueue enqueue threads\n");
        thread_spawn("workqueue_enqueue_thread", enqueue_thread, NULL);
    }

    printf("yielding\n");
    thread_apply_cpu_penalty(thread_get_current());
    while (atomic_load(&threads_left) > 0) {
        scheduler_yield();
    }

    uint64_t workers = wq->num_workers;

    char *msg = kmalloc(100, ALLOC_PARAMS_DEFAULT);
    snprintf(msg, 100, "There are %d workers", workers);
    ADD_MESSAGE(msg);

    printf("destroy\n");
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

TEST_REGISTER(daemon_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct cpu_mask cmask;
    cpu_mask_init(&cmask, global.core_count);
    cpu_mask_set_all(&cmask);

    struct daemon_attributes attrs = {
        .max_timesharing_threads = 67,
        .flags = DAEMON_FLAG_AUTO_SPAWN | DAEMON_FLAG_HAS_NAME,
        .thread_cpu_mask = cmask,
    };

    struct daemon *daemon =
        daemon_create("daemon_test", &attrs, &dwork, NULL, NULL);

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

    thread_sleep(thread_get_current(), THREAD_SLEEP_REASON_MANUAL,
                 THREAD_WAIT_INTERRUPTIBLE, (void *) 4);

    thread_wait_for_wake_match();

    atomic_store(&si_ok, true);
}

static void waking_thread(void *) {
    while (!atomic_load(&si_apc_ran))
        scheduler_yield();

    thread_wake(si_t, THREAD_WAKE_REASON_SLEEP_MANUAL,
                si_t->perceived_prio_class, (void *) 4);
}

TEST_REGISTER(thread_sleep_interruptible_test, SHOULD_NOT_FAIL,
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

static atomic_bool gogo = false;
static atomic_bool eq = false;

static void dpc_idle(struct dpc *dpc, void *ctx) {
    (void) dpc, (void) ctx;

    atomic_store(&gogo, true);
    kassert(scheduler_core_idle(smp_core()));
}

static void dpc_on_event_dummy_thread(void *a) {
    (void) a;
    while (!atomic_load(&eq))
        cpu_relax();

    for (size_t i = 0; i < 5000; i++)
        scheduler_yield();

    kassert(!atomic_load(&gogo));
}

/* we put a thread on a core that is not idle, enqueue a DPC over
 * there, trigger some reschedules, and then verify that the DPC
 * only ever runs once the core actually goes idle */
TEST_REGISTER(dpc_on_event_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    size_t i;
    size_t found = SIZE_MAX;
    for_each_cpu_id(i) {
        if (scheduler_core_idle(global.cores[i])) {
            found = i;
            break;
        }
    }

    if (found == SIZE_MAX) {
        ADD_MESSAGE("Could not find idle CPU");
        SET_SKIP();
        return;
    }

    struct thread *t =
        thread_create("dpc_dummy", dpc_on_event_dummy_thread, NULL);
    t->flags = THREAD_FLAGS_NO_STEAL;
    thread_enqueue_on_core(t, found);

    /* we now know the other processor is in the thread */
    struct dpc *dp = dpc_create(dpc_idle, NULL);
    dpc_enqueue_on_cpu(found, dp, DPC_CPU_IDLE);
    atomic_store(&eq, true);
    while (!atomic_load(&gogo))
        cpu_relax();

    SET_SUCCESS();
}

#define SCHED_PUSH_TEST_THREADS 256

static atomic_uint left = SCHED_PUSH_TEST_THREADS;
static atomic_bool at_least_one_migrated = false;

static void sched_push_try(void *) {
    while (smp_core_id() == 0 && !atomic_load(&at_least_one_migrated))
        scheduler_yield();

    atomic_fetch_sub(&left, 1);
    atomic_store(&at_least_one_migrated, true);
}

TEST_REGISTER(sched_push_target_test, SHOULD_NOT_FAIL, IS_INTEGRATION_TEST) {
    ADD_MESSAGE("This test takes a bit. uncomment me to run it");
    SET_SKIP();
    return;
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < SCHED_PUSH_TEST_THREADS; i++) {
        thread_spawn_on_core("push_test_%zu", sched_push_try, NULL, 0, i);
    }
    irql_lower(irql);

    while (!atomic_load(&left))
        scheduler_yield();

    SET_SUCCESS();
}

#endif
