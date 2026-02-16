#ifdef TEST_APC
#include <sch/sched.h>
#include <sleep.h>
#include <tests.h>
#include <thread/apc.h>
#include <thread/defer.h>
#include <thread/reaper.h>
#include <thread/thread.h>

static atomic_bool apc_ran = false;
static void the_apc(struct apc *a, void *arg1, void *arg2) {
    (void) arg1, (void) a, (void) arg2;
    atomic_store(&apc_ran, true);
}

static void apc_thread(void *) {
    while (!atomic_load(&apc_ran))
        scheduler_yield();
}

static struct thread *ted = NULL;
TEST_REGISTER(apc_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ted = thread_spawn("apc_test_thread", apc_thread, NULL);
    struct apc *a = kzalloc(sizeof(struct apc), ALLOC_PARAMS_DEFAULT);
    if (!a || !ted)
        goto pluh;

    apc_init(a, the_apc, NULL, NULL);

    apc_enqueue(ted, a, APC_TYPE_KERNEL);

    while (!atomic_load(&apc_ran))
        scheduler_yield();

pluh:
    SET_SUCCESS();
}

static atomic_uint the_event_apc_ran_times = 0;
static atomic_bool event_apc_test_ok = false;
static void the_event_apc(struct apc *pc, void *a, void *b) {
    (void) pc, (void) a, (void) b;
    atomic_fetch_add(&the_event_apc_ran_times, 1);
}

APC_EVENT_CREATE(apc_event_test, "TEST_EVENT");

static void apc_event_test_thread(void *) {
    /* We want to enqueue an event APC, then raise to DISPATCH, trigger it a
     * few times, check that no APCs got triggered, and then lower from there,
     * and then check that APCs got triggered, and then test masking, etc. */
    struct apc *evtapc = apc_create();
    apc_init(evtapc, the_event_apc, NULL, NULL);
    apc_enqueue_event_apc(evtapc, APC_EVENT(apc_event_test));

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT(atomic_load(&the_event_apc_ran_times) == 0);
    irql_lower(old);

    TEST_ASSERT(atomic_load(&the_event_apc_ran_times) == 1);

    apc_disable_kernel();
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT(atomic_load(&the_event_apc_ran_times) == 1);
    apc_enable_kernel();

    TEST_ASSERT(atomic_load(&the_event_apc_ran_times) == 2);
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT(atomic_load(&the_event_apc_ran_times) == 3);
    atomic_store(&event_apc_test_ok, true);
}

static struct thread *ated = NULL;
TEST_REGISTER(apc_event_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    ated = thread_spawn("apc_event_test_thread", apc_event_test_thread, NULL);
    while (!atomic_load(&event_apc_test_ok))
        scheduler_yield();

    SET_SUCCESS();
}

#endif
