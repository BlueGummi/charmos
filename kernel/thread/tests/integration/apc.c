#include "../test_internal.h"

#ifdef TEST_APC
TEST_GROUP_DECLARE(apc, .intensity_desc = {
                            .curve = SCALE_PIECEWISE_LOG,
                            .unit = "iters",
                        });

static atomic_bool apc_ran = false;
static atomic_uint apc_destroyed = 0;

static void the_apc(void *) {
    atomic_store(&apc_ran, true);
}

static void the_apc_destroy(struct apc *apc) {
    atomic_fetch_add(&apc_destroyed, 1);
    kfree(apc);
}

static void apc_thread(void *) {
    while (!atomic_load(&apc_ran))
        cpu_relax();
}

static struct thread *ted = NULL;
TEST_DECLARE_INTEGRATION(apc_test, .group = TEST_GROUP(apc)) {
    atomic_store(&apc_ran, false);
    atomic_store(&apc_destroyed, 0);
    ted = thread_spawn_joinable("apc_test_thread", apc_thread, NULL);
    struct apc *a = kmalloc(sizeof(struct apc), ALLOC_FLAGS_ZERO);
    if (!a || !ted) {
        if (a)
            kfree(a);
        if (ted)
            thread_detach(ted);

        return TEST_FAIL("allocation failed");
    }

    apc_init(a, the_apc, NULL, the_apc_destroy);

    TEST_ASSERT(thread_get(ted));
    TEST_ASSERT(apc_enqueue(ted, a, APC_TYPE_KERNEL));
    thread_put(ted);
    apc_put(a);

    /* the thread only returns once it has seen the APC run */
    thread_join(ted);
    TEST_ASSERT(atomic_load(&apc_ran));
    TEST_ASSERT(atomic_load(&apc_destroyed) == 1);

    return TEST_SUCCESS;
}

static atomic_uint apc_ref_destroyed = 0;

static void apc_ref_destroy(struct apc *apc) {
    (void) apc;
    atomic_fetch_add(&apc_ref_destroyed, 1);
}

TEST_DECLARE_INTEGRATION(apc_refcount_finalizes_at_zero,
                         .group = TEST_GROUP(apc)) {
    struct apc apc;
    atomic_store(&apc_ref_destroyed, 0);
    apc_init(&apc, the_apc, NULL, apc_ref_destroy);

    TEST_ASSERT(apc_get(&apc));
    apc_put(&apc);
    TEST_ASSERT(atomic_load(&apc_ref_destroyed) == 0);
    apc_put(&apc);
    TEST_ASSERT(atomic_load(&apc_ref_destroyed) == 1);
    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(apc_null_destroy_is_valid, .group = TEST_GROUP(apc)) {
    struct apc apc;
    apc_init(&apc, the_apc, NULL, NULL);
    apc_put(&apc);
    return TEST_SUCCESS;
}

static atomic_bool apc_cancel_ready = false;
static atomic_bool apc_cancel_release = false;
static atomic_bool apc_cancel_ran = false;
static atomic_uint apc_cancel_destroyed = 0;

static void cancelled_apc(void *arg) {
    (void) arg;
    atomic_store(&apc_cancel_ran, true);
}

static void cancelled_apc_destroy(struct apc *apc) {
    atomic_fetch_add(&apc_cancel_destroyed, 1);
    kfree(apc);
}

static void apc_cancel_target(void *arg) {
    (void) arg;
    apc_disable_kernel();
    atomic_store(&apc_cancel_ready, true);
    while (!atomic_load(&apc_cancel_release))
        scheduler_yield();
}

TEST_DECLARE_INTEGRATION(apc_cancel_releases_queue_reference,
                         .group = TEST_GROUP(apc)) {
    atomic_store(&apc_cancel_ready, false);
    atomic_store(&apc_cancel_release, false);
    atomic_store(&apc_cancel_ran, false);
    atomic_store(&apc_cancel_destroyed, 0);

    struct thread *target =
        thread_spawn_joinable("apc_cancel_target", apc_cancel_target, NULL);
    TEST_ASSERT(target);
    while (!atomic_load(&apc_cancel_ready))
        scheduler_yield();

    struct apc *apc = apc_create();
    TEST_ASSERT(apc);
    apc_init(apc, cancelled_apc, NULL, cancelled_apc_destroy);

    TEST_ASSERT(thread_get(target));
    TEST_ASSERT(apc_enqueue(target, apc, APC_TYPE_KERNEL));
    TEST_ASSERT(!apc_enqueue(target, apc, APC_TYPE_KERNEL));
    TEST_ASSERT(apc_cancel(target, apc));
    thread_put(target);

    apc_put(apc);
    TEST_ASSERT(atomic_load(&apc_cancel_destroyed) == 1);
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
    (void) arg;
    atomic_store(&apc_rundown_ran, true);
}

static void rundown_apc_destroy(struct apc *apc) {
    atomic_fetch_add(&apc_rundown_destroyed, 1);
    kfree(apc);
}

static void apc_rundown_target(void *arg) {
    (void) arg;
    apc_disable_kernel();
    atomic_store(&apc_rundown_ready, true);
    while (!atomic_load(&apc_rundown_release))
        scheduler_yield();
}

TEST_DECLARE_INTEGRATION(apc_thread_rundown_releases_queue_reference,
                         .group = TEST_GROUP(apc)) {
    atomic_store(&apc_rundown_ready, false);
    atomic_store(&apc_rundown_release, false);
    atomic_store(&apc_rundown_ran, false);
    atomic_store(&apc_rundown_destroyed, 0);

    struct thread *target =
        thread_spawn_joinable("apc_rundown_target", apc_rundown_target, NULL);
    TEST_ASSERT(target);
    while (!atomic_load(&apc_rundown_ready))
        scheduler_yield();

    struct apc *apc = apc_create();
    TEST_ASSERT(apc);
    apc_init(apc, rundown_apc, NULL, rundown_apc_destroy);

    TEST_ASSERT(thread_get(target));
    TEST_ASSERT(apc_enqueue(target, apc, APC_TYPE_KERNEL));
    thread_put(target);
    apc_put(apc);

    atomic_store(&apc_rundown_release, true);
    thread_join(target);

    TEST_ASSERT(!atomic_load(&apc_rundown_ran));
    TEST_ASSERT(atomic_load(&apc_rundown_destroyed) == 1);
    return TEST_SUCCESS;
}

static atomic_uint apc_reuse_ran = 0;
static atomic_uint apc_reuse_destroyed = 0;

static void reused_apc(void *arg) {
    (void) arg;
    atomic_fetch_add(&apc_reuse_ran, 1);
}

static void reused_apc_destroy(struct apc *apc) {
    (void) apc;
    atomic_fetch_add(&apc_reuse_destroyed, 1);
}

static void apc_reuse_target(void *arg) {
    (void) arg;
    while (atomic_load(&apc_reuse_ran) < 2)
        scheduler_yield();
}

TEST_DECLARE_INTEGRATION(apc_caller_reference_allows_reuse,
                         .group = TEST_GROUP(apc)) {
    atomic_store(&apc_reuse_ran, 0);
    atomic_store(&apc_reuse_destroyed, 0);

    struct thread *target =
        thread_spawn_joinable("apc_reuse_target", apc_reuse_target, NULL);
    TEST_ASSERT(target);

    struct apc apc;
    apc_init(&apc, reused_apc, NULL, reused_apc_destroy);
    for (size_t expected = 1; expected <= 2; expected++) {
        TEST_ASSERT(thread_get(target));
        TEST_ASSERT(apc_enqueue(target, &apc, APC_TYPE_KERNEL));
        thread_put(target);
        while (atomic_load(&apc_reuse_ran) < expected)
            scheduler_yield();
        while (atomic_load_explicit(&apc.state, memory_order_acquire) !=
               APC_STATE_IDLE)
            scheduler_yield();
    }

    thread_join(target);
    TEST_ASSERT(atomic_load(&apc_reuse_ran) == 2);
    TEST_ASSERT(atomic_load(&apc_reuse_destroyed) == 0);
    apc_put(&apc);
    TEST_ASSERT(atomic_load(&apc_reuse_destroyed) == 1);
    return TEST_SUCCESS;
}

static atomic_bool apc_race_ready = false;
static atomic_bool apc_race_release = false;
static atomic_bool apc_race_settled = false;
static atomic_uint apc_race_ran = 0;
static atomic_uint apc_race_destroyed = 0;

static void raced_apc(void *arg) {
    (void) arg;
    atomic_fetch_add(&apc_race_ran, 1);
}

static void raced_apc_destroy(struct apc *apc) {
    atomic_fetch_add(&apc_race_destroyed, 1);
    kfree(apc);
}

static void apc_race_target(void *arg) {
    (void) arg;
    apc_disable_kernel();
    atomic_store(&apc_race_ready, true);
    while (!atomic_load(&apc_race_release))
        scheduler_yield();
    apc_enable_kernel();
    while (!atomic_load(&apc_race_settled))
        scheduler_yield();
}

TEST_DECLARE_INTEGRATION(apc_cancel_races_delivery, .group = TEST_GROUP(apc)) {
    atomic_store(&apc_race_ready, false);
    atomic_store(&apc_race_release, false);
    atomic_store(&apc_race_settled, false);
    atomic_store(&apc_race_ran, 0);
    atomic_store(&apc_race_destroyed, 0);

    struct thread *target =
        thread_spawn_joinable("apc_race_target", apc_race_target, NULL);
    TEST_ASSERT(target);
    while (!atomic_load(&apc_race_ready))
        scheduler_yield();

    struct apc *apc = apc_create();
    TEST_ASSERT(apc);
    apc_init(apc, raced_apc, NULL, raced_apc_destroy);
    TEST_ASSERT(thread_get(target));
    TEST_ASSERT(apc_enqueue(target, apc, APC_TYPE_KERNEL));

    atomic_store(&apc_race_release, true);
    bool cancelled = apc_cancel(target, apc);
    thread_put(target);
    atomic_store(&apc_race_settled, true);
    apc_put(apc);
    thread_join(target);

    TEST_ASSERT(atomic_load(&apc_race_ran) == (cancelled ? 0 : 1));
    TEST_ASSERT(atomic_load(&apc_race_destroyed) == 1);
    return TEST_SUCCESS;
}

static atomic_uint the_event_apc_ran_times = 0;
static atomic_bool event_apc_test_ok = false;
static void the_event_apc(void *pc) {
    atomic_fetch_add(&the_event_apc_ran_times, 1);
}

APC_EVENT_CREATE(apc_event_test, "TEST_EVENT");

static void apc_event_test_thread(void *) {
    /* We want to enqueue an event APC, then raise to DISPATCH, trigger it a
     * few times, check that no APCs got triggered, and then lower from there,
     * and then check that APCs got triggered, and then test masking, etc. */
    struct event_apc *evtapc = apc_event_apc_create();
    apc_event_apc_init(evtapc, the_event_apc, NULL, apc_destroy_free);
    TEST_ASSERT_VOID(apc_enqueue_event_apc(evtapc, APC_EVENT(apc_event_test)));
    apc_put(&evtapc->apc);

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT_VOID(atomic_load(&the_event_apc_ran_times) == 0);
    irql_lower(old);

    TEST_ASSERT_VOID(atomic_load(&the_event_apc_ran_times) == 1);

    apc_disable_kernel();
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT_VOID(atomic_load(&the_event_apc_ran_times) == 1);
    apc_enable_kernel();

    TEST_ASSERT_VOID(atomic_load(&the_event_apc_ran_times) == 2);
    apc_event_signal(APC_EVENT(apc_event_test));
    TEST_ASSERT_VOID(atomic_load(&the_event_apc_ran_times) == 3);
    atomic_store(&event_apc_test_ok, true);
}

static struct thread *ated = NULL;
TEST_DECLARE_INTEGRATION(apc_event_test, .group = TEST_GROUP(apc)) {
    atomic_store(&the_event_apc_ran_times, 0);
    atomic_store(&event_apc_test_ok, false);

    ated = thread_spawn_joinable("apc_event_test_thread", apc_event_test_thread,
                                 NULL);
    TEST_ASSERT(ated);

    /* joining rather than spinning on the ok flag means a failed
     * TEST_ASSERT_VOID inside the thread reports instead of hanging */
    thread_join(ated);
    TEST_ASSERT(atomic_load(&event_apc_test_ok));

    return TEST_SUCCESS;
}
#endif
