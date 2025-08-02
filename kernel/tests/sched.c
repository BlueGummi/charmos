#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
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
static void event_pool_fn(void *arg, void *unused) {
    (void) arg, (void) unused;
    atomic_store(&event_pool_ran, true);
    ADD_MESSAGE("event pool ran");
}

REGISTER_TEST(event_pool_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    event_pool_add_remote(event_pool_fn, NULL, NULL);
    sleep_ms(50);
    TEST_ASSERT(atomic_load(&event_pool_ran));

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

    uint64_t start_time = me->time_in_level;
    for (uint64_t i = 0; i < spins; i++) {
        /* This sleep function just
         * busy wait-polls the timer */
        sleep_ms(1);

        /* This is changed if preemption occurred */
        if (me->time_in_level != start_time) {
            goto fail;
        }
    }

    return;

fail:
    atomic_store(&rt_thread_fail, true);
    return;
}

REGISTER_TEST(rt_thread_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct thread *thread = thread_create(rt_thread);
    rt = thread;
    thread->base_prio = THREAD_PRIO_URGENT;
    thread->perceived_prio = THREAD_PRIO_URGENT;

    scheduler_enqueue_on_core(thread, get_this_core_id());
    scheduler_yield();
    TEST_ASSERT(!atomic_load(&rt_thread_fail));

    SET_SUCCESS;
}

static atomic_bool ts_thread_fail = false;
static struct thread *ts = NULL;
static void ts_thread(void) {
    uint64_t spins = 10;
    struct thread *me = scheduler_get_curr_thread();
    if (me != ts) {
        goto fail;
    }

    uint64_t start_time = me->time_in_level;
    enum thread_priority start_prio = me->perceived_prio;

    for (uint64_t i = 0; i < spins; i++)
        wait_for_interrupt();

    /* Time in level never changed, and priority level never changed */
    if (me->time_in_level == start_time && start_prio == me->perceived_prio)
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
    uint64_t reaped_threads_at_start = reaper_get_reaped_thread_count();
    disable_interrupts();
    uint32_t run_times = 500;

    for (uint32_t i = 0; i < run_times; i++)
        thread_spawn(sched_spawn_entry);

    enable_interrupts();

    while (run_times != ran_count)
        scheduler_yield();

    TEST_ASSERT(run_times == ran_count);

    while (reaper_get_reaped_thread_count() !=
           reaped_threads_at_start + run_times)
        sleep_ms(1);

    SET_SUCCESS;
}
