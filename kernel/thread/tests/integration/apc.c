#include "thread/tests/test_internal.h"

TEST_GROUP_DEFINE(apc, .intensity_desc = {
                           .curve = SCALE_PIECEWISE_LOG,
                           .unit = "iters",
                       });

static atomic_bool apc_ran = false;
static atomic_uint apc_destroyed = 0;

static void the_apc(void *arg) {
    cc_ignore(arg);
    atomic_store(&apc_ran, true);
}

static void the_apc_destroy(struct apc *apc) {
    atomic_inc(&apc_destroyed);
    vfree(apc);
}

static void apc_thread(void *arg) {
    cc_ignore(arg);
    while (!atomic_load(&apc_ran))
        cpu_pause();
}

static struct thread *ted = NULL;
TEST_DEFINE_INTEGRATION(apc, kernel_apc_runs_then_destroys) {
    atomic_store(&apc_ran, false);
    atomic_store(&apc_destroyed, 0);
    ted = thread_spawn("apc_test_thread", apc_thread, .joinable = true);
    struct apc *a = vnew(a, ALLOC_ZERO);
    if (!a || !ted) {
        if (a)
            vfree(a);
        if (ted)
            thread_detach(ted);

        return TEST_FAIL("allocation failed");
    }

    apc_init(a, the_apc, NULL, the_apc_destroy);

    TEST_ASSERT(thread_getref(ted));
    TEST_ASSERT(apc_enqueue(ted, a, APC_TYPE_KERNEL));
    thread_putref(ted);
    apc_putref(a);

    /* the thread only returns once it has seen the APC run */
    thread_join(ted);
    TEST_ASSERT(atomic_load(&apc_ran));
    TEST_ASSERT_EQ(atomic_load(&apc_destroyed), 1);

    return TEST_SUCCESS;
}

static atomic_uint apc_ref_destroyed = 0;

static void apc_ref_destroy(struct apc *apc) {
    cc_ignore(apc);
    atomic_inc(&apc_ref_destroyed);
}

TEST_DEFINE_INTEGRATION(apc, refcount_finalizes_at_zero) {
    struct apc apc;
    atomic_store(&apc_ref_destroyed, 0);
    apc_init(&apc, the_apc, NULL, apc_ref_destroy);

    TEST_ASSERT(apc_getref(&apc));
    apc_putref(&apc);
    TEST_ASSERT_EQ(atomic_load(&apc_ref_destroyed), 0);
    apc_putref(&apc);
    TEST_ASSERT_EQ(atomic_load(&apc_ref_destroyed), 1);
    return TEST_SUCCESS;
}

TEST_DEFINE_INTEGRATION(apc, null_destroy_valid) {
    struct apc apc;
    apc_init(&apc, the_apc, NULL, NULL);
    apc_putref(&apc);
    return TEST_SUCCESS;
}

static atomic_bool apc_cancel_ready = false;
static atomic_bool apc_cancel_release = false;
static atomic_bool apc_cancel_ran = false;
static atomic_uint apc_cancel_destroyed = 0;

static void cancelled_apc(void *arg) {
    cc_ignore(arg);
    atomic_store(&apc_cancel_ran, true);
}

static void cancelled_apc_destroy(struct apc *apc) {
    atomic_inc(&apc_cancel_destroyed);
    vfree(apc);
}

static void apc_cancel_target(void *arg) {
    cc_ignore(arg);
    apc_disable_kernel();
    atomic_store(&apc_cancel_ready, true);
    while (!atomic_load(&apc_cancel_release))
        scheduler_yield();
}

TEST_DEFINE_INTEGRATION(apc, cancel_releases_queue_ref) {
    atomic_store(&apc_cancel_ready, false);
    atomic_store(&apc_cancel_release, false);
    atomic_store(&apc_cancel_ran, false);
    atomic_store(&apc_cancel_destroyed, 0);

    struct thread *target =
        thread_spawn("apc_cancel_target", apc_cancel_target, .joinable = true);
    TEST_ASSERT_NONNULL(target);
    while (!atomic_load(&apc_cancel_ready))
        scheduler_yield();

    struct apc *apc = apc_create();
    TEST_ASSERT_NONNULL(apc);
    apc_init(apc, cancelled_apc, NULL, cancelled_apc_destroy);

    TEST_ASSERT(thread_getref(target));
    TEST_ASSERT(apc_enqueue(target, apc, APC_TYPE_KERNEL));
    TEST_ASSERT(!apc_enqueue(target, apc, APC_TYPE_KERNEL));
    TEST_ASSERT(apc_cancel(target, apc));
    thread_putref(target);

    apc_putref(apc);
    TEST_ASSERT_EQ(atomic_load(&apc_cancel_destroyed), 1);
    TEST_ASSERT(!atomic_load(&apc_cancel_ran));

    atomic_store(&apc_cancel_release, true);
    thread_join(target);
    return TEST_SUCCESS;
}

static atomic_bool apc_rundown_ready = false;
static atomic_bool apc_rundown_release = false;
static atomic_bool apc_rundown_ran = false;
static atomic_uint apc_rundown_destroyed = 0;

static void rundown_apc(void *arg) {
    cc_ignore(arg);
    atomic_store(&apc_rundown_ran, true);
}

static void rundown_apc_destroy(struct apc *apc) {
    atomic_inc(&apc_rundown_destroyed);
    vfree(apc);
}

static void apc_rundown_target(void *arg) {
    cc_ignore(arg);
    apc_disable_kernel();
    atomic_store(&apc_rundown_ready, true);
    while (!atomic_load(&apc_rundown_release))
        scheduler_yield();
}

TEST_DEFINE_INTEGRATION(apc, thread_rundown_releases_queue_ref) {
    atomic_store(&apc_rundown_ready, false);
    atomic_store(&apc_rundown_release, false);
    atomic_store(&apc_rundown_ran, false);
    atomic_store(&apc_rundown_destroyed, 0);

    struct thread *target = thread_spawn("apc_rundown_target",
                                         apc_rundown_target, .joinable = true);
    TEST_ASSERT_NONNULL(target);
    while (!atomic_load(&apc_rundown_ready))
        scheduler_yield();

    struct apc *apc = apc_create();
    TEST_ASSERT_NONNULL(apc);
    apc_init(apc, rundown_apc, NULL, rundown_apc_destroy);

    TEST_ASSERT(thread_getref(target));
    TEST_ASSERT(apc_enqueue(target, apc, APC_TYPE_KERNEL));
    thread_putref(target);
    apc_putref(apc);

    atomic_store(&apc_rundown_release, true);
    thread_join(target);

    TEST_ASSERT(!atomic_load(&apc_rundown_ran));
    TEST_ASSERT_EQ(atomic_load(&apc_rundown_destroyed), 1);
    return TEST_SUCCESS;
}

static atomic_uint apc_reuse_ran = 0;
static atomic_uint apc_reuse_destroyed = 0;

static void reused_apc(void *arg) {
    cc_ignore(arg);
    atomic_inc(&apc_reuse_ran);
}

static void reused_apc_destroy(struct apc *apc) {
    cc_ignore(apc);
    atomic_inc(&apc_reuse_destroyed);
}

static void apc_reuse_target(void *arg) {
    cc_ignore(arg);
    while (atomic_load(&apc_reuse_ran) < 2)
        scheduler_yield();
}

TEST_DEFINE_INTEGRATION(apc, caller_ref_allows_reuse) {
    atomic_store(&apc_reuse_ran, 0);
    atomic_store(&apc_reuse_destroyed, 0);

    struct thread *target =
        thread_spawn("apc_reuse_target", apc_reuse_target, .joinable = true);
    TEST_ASSERT_NONNULL(target);

    struct apc apc;
    apc_init(&apc, reused_apc, NULL, reused_apc_destroy);
    for (size_t expected = 1; expected <= 2; expected++) {
        TEST_ASSERT(thread_getref(target));
        TEST_ASSERT(apc_enqueue(target, &apc, APC_TYPE_KERNEL));
        thread_putref(target);
        while (atomic_load(&apc_reuse_ran) < expected)
            scheduler_yield();
        while (atomic_load_acq(&apc.state) != APC_STATE_IDLE)
            scheduler_yield();
    }

    thread_join(target);
    TEST_ASSERT_EQ(atomic_load(&apc_reuse_ran), 2);
    TEST_ASSERT_EQ(atomic_load(&apc_reuse_destroyed), 0);
    apc_putref(&apc);
    TEST_ASSERT_EQ(atomic_load(&apc_reuse_destroyed), 1);
    return TEST_SUCCESS;
}

static atomic_bool apc_race_ready = false;
static atomic_bool apc_race_release = false;
static atomic_bool apc_race_settled = false;
static atomic_uint apc_race_ran = 0;
static atomic_uint apc_race_destroyed = 0;

static void raced_apc(void *arg) {
    cc_ignore(arg);
    atomic_inc(&apc_race_ran);
}

static void raced_apc_destroy(struct apc *apc) {
    atomic_inc(&apc_race_destroyed);
    vfree(apc);
}

static void apc_race_target(void *arg) {
    cc_ignore(arg);
    apc_disable_kernel();
    atomic_store(&apc_race_ready, true);
    while (!atomic_load(&apc_race_release))
        scheduler_yield();
    apc_enable_kernel();
    while (!atomic_load(&apc_race_settled))
        scheduler_yield();
}

TEST_DEFINE_INTEGRATION(apc, cancel_races_delivery) {
    atomic_store(&apc_race_ready, false);
    atomic_store(&apc_race_release, false);
    atomic_store(&apc_race_settled, false);
    atomic_store(&apc_race_ran, 0);
    atomic_store(&apc_race_destroyed, 0);

    struct thread *target =
        thread_spawn("apc_race_target", apc_race_target, .joinable = true);
    TEST_ASSERT_NONNULL(target);
    while (!atomic_load(&apc_race_ready))
        scheduler_yield();

    struct apc *apc = apc_create();
    TEST_ASSERT_NONNULL(apc);
    apc_init(apc, raced_apc, NULL, raced_apc_destroy);
    TEST_ASSERT(thread_getref(target));
    TEST_ASSERT(apc_enqueue(target, apc, APC_TYPE_KERNEL));

    atomic_store(&apc_race_release, true);
    bool cancelled = apc_cancel(target, apc);
    thread_putref(target);
    atomic_store(&apc_race_settled, true);
    apc_putref(apc);
    thread_join(target);

    TEST_ASSERT_EQ(atomic_load(&apc_race_ran), (cancelled ? 0 : 1));
    TEST_ASSERT_EQ(atomic_load(&apc_race_destroyed), 1);
    return TEST_SUCCESS;
}

static atomic_uint the_event_apc_ran_times = 0;
static atomic_bool event_apc_test_ok = false;
static void the_event_apc(void *pc) {
    cc_ignore(pc);
    atomic_inc(&the_event_apc_ran_times);
}

APC_EVENT_CREATE(apc_event_test, "TEST_EVENT");

static void apc_event_test_thread(void *arg) TSA_NO_ANALYSIS {
    cc_ignore(arg);
    /* We want to enqueue an event APC, then raise to DISPATCH, trigger it a
     * few times, check that no APCs got triggered, and then lower from there,
     * and then check that APCs got triggered, and then test masking, etc. */
    struct event_apc *evtapc = apc_event_apc_create();
    apc_event_apc_init(evtapc, the_event_apc, NULL, apc_destroy_free);
    TEST_ASSERT_VOID(apc_enqueue_event_apc(evtapc, APC_EVENT(apc_event_test)));
    apc_putref(&evtapc->apc);

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT_VOID_EQ(atomic_load(&the_event_apc_ran_times), 0);
    irql_lower(old);

    TEST_ASSERT_VOID_EQ(atomic_load(&the_event_apc_ran_times), 1);

    apc_disable_kernel();
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT_VOID_EQ(atomic_load(&the_event_apc_ran_times), 1);
    apc_enable_kernel();

    TEST_ASSERT_VOID_EQ(atomic_load(&the_event_apc_ran_times), 2);
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT_VOID_EQ(atomic_load(&the_event_apc_ran_times), 3);
    atomic_store(&event_apc_test_ok, true);
}

static struct thread *ated = NULL;
TEST_DEFINE_INTEGRATION(apc, event_masking_and_signal) {
    atomic_store(&the_event_apc_ran_times, 0);
    atomic_store(&event_apc_test_ok, false);

    ated = thread_spawn("apc_event_test_thread", apc_event_test_thread,
                        .joinable = true);
    TEST_ASSERT_NONNULL(ated);

    /* joining rather than spinning on the ok flag means a failed
     * TEST_ASSERT_VOID inside the thread reports instead of hanging */
    thread_join(ated);
    TEST_ASSERT(atomic_load(&event_apc_test_ok));

    return TEST_SUCCESS;
}
