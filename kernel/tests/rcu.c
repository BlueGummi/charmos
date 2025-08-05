#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <tests.h>
#include <types/rcu.h>

#define NUM_RCU_READERS (global.core_count)
#define RCU_TEST_DURATION_MS 50

struct rcu_test_data {
    int value;
};

static struct rcu_test_data *volatile shared_ptr = NULL;
static atomic_bool rcu_test_failed = false;
static atomic_uint rcu_reads_done = 0;

static void rcu_reader_thread(void) {
    disable_interrupts();
    uint64_t end = time_get_ms() + RCU_TEST_DURATION_MS;

    while (time_get_ms() < end) {
        rcu_read_lock();

        struct rcu_test_data *p = shared_ptr;
        if (p) {
            int v = p->value;
            if (v != 42 && v != 43) {
                atomic_store(&rcu_test_failed, true);
                ADD_MESSAGE("RCU reader saw invalid value");
                k_printf("%d\n", v);
            }
        }

        rcu_read_unlock();
    }

    atomic_fetch_add(&rcu_reads_done, 1);
    k_printf("Reader thread complete\n");
    enable_interrupts();
}

static atomic_bool volatile rcu_deferred_freed = false;

static void rcu_free_fn(void *ptr) {
    kfree(ptr);

    atomic_store(&rcu_deferred_freed, true);
}

static void rcu_writer_thread(void) {
    sleep_ms(30);

    struct rcu_test_data *old = shared_ptr;

    struct rcu_test_data *new = kmalloc(sizeof(*new));
    new->value = 43;
    shared_ptr = new;

    rcu_synchronize();
    rcu_defer(rcu_free_fn, old);
    k_printf("Writer thread complete\n");
}

REGISTER_TEST(rcu_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    enable_interrupts();
    struct rcu_test_data *initial = kmalloc(sizeof(*initial));
    initial->value = 42;
    shared_ptr = initial;

    for (uint64_t i = 0; i < NUM_RCU_READERS; i++)
        thread_spawn(rcu_reader_thread);

    k_printf("Readers spawned\n");
    thread_spawn(rcu_writer_thread);

    while (atomic_load(&rcu_reads_done) < NUM_RCU_READERS)
        scheduler_yield();

    for (int i = 0; i < 100 && !atomic_load(&rcu_deferred_freed); i++)
        sleep_ms(1);

    TEST_ASSERT(!atomic_load(&rcu_test_failed));
    while (!atomic_load(&rcu_deferred_freed))
        ;

    SET_SUCCESS;
}
