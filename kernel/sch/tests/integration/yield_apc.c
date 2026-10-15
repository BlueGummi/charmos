#include "sch/tests/test_internal.h"

#define YIELD_APC_SPIN_LIMIT 2000000
#define YIELD_APC_NO_READING 999

static atomic_bool yd_apc_ran = false;
static atomic_uint yd_nesting_at_delivery = YIELD_APC_NO_READING;
static atomic_bool yd_gave_up = false;

static void yd_apc(void *arg) {
    cc_ignore(arg);
    atomic_store(&yd_nesting_at_delivery,
                 scheduler_yield_nesting(thread_get_current()));
    atomic_store(&yd_apc_ran, true);
}

static void yd_subject_main(void *arg) {
    cc_ignore(arg);
    for (size_t i = 0; i < YIELD_APC_SPIN_LIMIT; i++) {
        if (atomic_load(&yd_apc_ran))
            return;
        scheduler_yield();
    }
    atomic_store(&yd_gave_up, true);
}

static void yd_enqueue_main(void *arg) {
    struct thread *target = arg;
    struct apc *apc = apc_create();
    if (!apc)
        return;

    apc_init(apc, yd_apc, NULL, apc_destroy_free);
    rcu_read_lock();
    bool got = thread_getref_rcu(target);
    rcu_read_unlock();

    if (got) {
        apc_enqueue(target, apc, APC_TYPE_KERNEL);
        thread_putref(target);
    }
    apc_putref(apc);
}

TEST_DEFINE_INTEGRATION(sched, yield_defers_kernel_apcs, .min_cores = 3) {
    atomic_store(&yd_apc_ran, false);
    atomic_store(&yd_gave_up, false);
    atomic_store(&yd_nesting_at_delivery, YIELD_APC_NO_READING);

    struct thread *subject = thread_spawn("yd_subject", yd_subject_main,
                                          .on_cpu = 1, .joinable = true);
    TEST_ASSERT_NONNULL(subject);

    struct thread *enq = thread_spawn("yd_enq", yd_enqueue_main, .arg = subject,
                                      .on_cpu = 2, .joinable = true);
    TEST_ASSERT_NONNULL(enq);

    thread_join(subject);
    thread_join(enq);

    TEST_ASSERT(!atomic_load(&yd_gave_up));
    TEST_ASSERT(atomic_load(&yd_apc_ran));

    TEST_ASSERT_EQ_S((int) atomic_load(&yd_nesting_at_delivery), 0);

    return TEST_SUCCESS;
}
