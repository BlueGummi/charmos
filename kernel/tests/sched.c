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
        SET_FAIL;
        return;
    }

    SET_SUCCESS;
}

static atomic_bool event_pool_ran = false;
static atomic_uint event_pool_times = 0;
static void event_pool_fn(void *arg, void *unused) {
    (void) arg, (void) unused;
    atomic_store(&event_pool_ran, true);
    atomic_fetch_add(&event_pool_times, 1);
}

REGISTER_TEST(event_pool_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint64_t tsc = rdtsc();
    uint64_t times = 256;

    for (uint64_t i = 0; i < times; i++)
        event_pool_add_fast(event_pool_fn, NULL, NULL);

    uint64_t total = rdtsc() - tsc;
    sleep_ms(50);

    while (!atomic_load(&event_pool_ran))
        cpu_relax();

    char *msg = kzalloc(100);
    TEST_ASSERT(msg);
    snprintf(msg, 100, "Took %d clock cycles to add to event pool %d times",
             total, times);
    ADD_MESSAGE(msg);
    TEST_ASSERT(atomic_load(&event_pool_ran));
    msg = kzalloc(100);
    snprintf(msg, 100,
             "Event pool ran %d times, tests should've had it run %d times",
             event_pool_times, times);
    ADD_MESSAGE(msg);

    SET_SUCCESS;
}

static atomic_bool rt_thread_fail = false;
static struct thread *rt = NULL;

static void rt_thread(void) {
    uint64_t spins = 50;
    struct thread *me = scheduler_get_curr_thread();
    if (me != rt) {
        k_printf("Different thread\n");
        goto fail;
    }

    for (uint64_t i = 0; i < spins; i++) {
        /* This sleep function just
         * busy wait-polls the timer */
        sleep_ms(1);

        /* This is changed if preemption occurred */
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

    scheduler_enqueue_on_core(thread, get_this_core_id());
    scheduler_yield();
    TEST_ASSERT(!atomic_load(&rt_thread_fail));

    SET_SUCCESS;
}

static atomic_bool ts_thread_fail = false;
static struct thread *ts = NULL;
static void ts_thread(void) {
    uint64_t spins = 100;
    struct thread *me = scheduler_get_curr_thread();
    if (me != ts) {
        goto fail;
    }

    thread_prio_t start_prio = me->priority_score;

    for (uint64_t i = 0; i < spins; i++)
        wait_for_interrupt();

    /* Time in level never changed, and priority level never changed */
    if (start_prio == me->priority_score)
        goto fail;

    return;

fail:
    atomic_store(&ts_thread_fail, true);
    return;
}

REGISTER_TEST(ts_thread_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {

    disable_interrupts();
    ts = thread_spawn(ts_thread);
    enable_interrupts();

    scheduler_yield();

    TEST_ASSERT(!atomic_load(&ts_thread_fail));

    SET_SUCCESS;
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

    SET_SUCCESS;
}
