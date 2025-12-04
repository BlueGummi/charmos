#include <sch/apc.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <tests.h>

static atomic_bool apc_ran = false;
static void the_apc(struct apc *a, void *arg1, void *arg2) {
    (void) arg1, (void) a, (void) arg2;
    atomic_store(&apc_ran, true);
}

static void apc_thread(void*) {
    while (!atomic_load(&apc_ran))
        scheduler_yield();
}

static struct thread *ted = NULL;
REGISTER_TEST(apc_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
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
