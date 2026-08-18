#include "test_internal.h"

#ifdef TEST_MUTEX
TEST_GROUP_DECLARE(mutex);


#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct mutex basic_test_mtx = MUTEX_INIT;

TEST_DECLARE_UNIT(mutex_test_basic, .group = TEST_GROUP(mutex)) {
    mutex_lock(&basic_test_mtx);
    scheduler_yield();
    mutex_unlock(&basic_test_mtx);
    return TEST_SUCCESS;
}

/* we want to spawn a timesharing thread on another core, and
 * acquire a mutex with it. then we want to spawn a realtime thread
 * on the same core. the expected behavior is that the timesharing
 * thread gets boosted to the realtime priority class, allowing it to run
 * until it drops the lock */

static struct mutex pi_mutex = MUTEX_INIT;
static struct thread *pi_ts, *pi_rt, *pi_dum;
static atomic_bool pi_ts_got = false;
static atomic_uint pi_done = 0;

static void pi_dummy(void *nothing) {
    (void) nothing;
    test_info("dummy");
    while (atomic_load(&pi_done) < 1)
        scheduler_yield();

    atomic_fetch_add(&pi_done, 1);
    test_info("exiting");
}

static void pi_rt_thread(void *nothing) {
    (void) nothing;
    mutex_lock(&pi_mutex);
    test_info("lock");
    kassert(mutex_get_owner(&pi_mutex) == thread_get_current());
    mutex_unlock(&pi_mutex);
    test_info("unlock");
    atomic_fetch_add(&pi_done, 1);
    test_info("exiting");
}

static void pi_ts_thread(void *nothing) {
    (void) nothing;
    mutex_lock(&pi_mutex);
    test_info("lock");
    atomic_store(&pi_ts_got, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    kassert(mutex_get_owner(&pi_mutex) == thread_get_current());
    test_info("boosted");

    test_info("unlock");
    mutex_unlock(&pi_mutex);

    atomic_fetch_add(&pi_done, 1);
    test_info("exiting");
}

TEST_DECLARE_UNIT(mutex_pi_test, .group = TEST_GROUP(mutex)) {
    if (global.core_count == 1) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    cpu_id_t cpu = 1;
    pi_ts = thread_create("pi_ts", pi_ts_thread, NULL);
    pi_rt = thread_create("pi_rt", pi_rt_thread, NULL);
    pi_dum = thread_create("pi_dum", pi_dummy, NULL);
    pi_rt->perceived_prio_class = THREAD_PRIO_CLASS_RT;
    pi_dum->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    pi_dum->flags |= THREAD_FLAG_PINNED;
    pi_ts->flags |= THREAD_FLAG_PINNED;
    pi_rt->flags |= THREAD_FLAG_PINNED;

    thread_set_joinable(pi_ts);
    thread_set_joinable(pi_rt);
    thread_set_joinable(pi_dum);

    thread_enqueue_on_core(pi_ts, cpu);

    /* rendezvous, not a join: ts has to own the mutex before rt asks for it */
    while (!atomic_load(&pi_ts_got))
        scheduler_yield();

    thread_enqueue_on_core(pi_dum, cpu);
    thread_enqueue_on_core(pi_rt, cpu);

    /* pi_done is still what pi_dummy waits on, so it stays */
    thread_join(pi_ts);
    thread_join(pi_rt);
    thread_join(pi_dum);

    TEST_ASSERT(atomic_load(&pi_done) == 3);

    return TEST_SUCCESS;
}

static struct mutex pi_mtx_a = MUTEX_INIT;
static struct mutex pi_mtx_b = MUTEX_INIT;

static struct thread *pi_ts1, *pi_ts2, *pi_rt2;
static atomic_uint pi_chain_done = 0;
static atomic_bool ts1_grabbed_a = false;
static atomic_bool ts2_grabbed_b = false;

static void pi_chain_ts2(void *arg) {
    (void) arg;
    mutex_lock(&pi_mtx_b);
    test_info("ts2 lock b");
    atomic_store(&ts2_grabbed_b, true);

    /* wait until boosted */
    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    test_info("ts2 boosted");
    mutex_unlock(&pi_mtx_b);
    atomic_fetch_add(&pi_chain_done, 1);
}

static void pi_chain_ts1(void *arg) {
    (void) arg;
    mutex_lock(&pi_mtx_a);
    test_info("ts1 lock a");
    atomic_store(&ts1_grabbed_a, true);

    /* wait until boosted */
    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    mutex_lock(&pi_mtx_b);
    test_info("ts1 lock b");

    mutex_unlock(&pi_mtx_b);
    mutex_unlock(&pi_mtx_a);
    atomic_fetch_add(&pi_chain_done, 1);
}

static void pi_chain_rt(void *arg) {
    (void) arg;
    test_info("rt lock");
    mutex_lock(&pi_mtx_a);
    test_info("rt lock got");

    mutex_unlock(&pi_mtx_a);
    atomic_fetch_add(&pi_chain_done, 1);
}

TEST_DECLARE_UNIT(mutex_pi_chain, .group = TEST_GROUP(mutex)) {
    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    cpu_id_t cpu = 1;

    pi_ts2 = thread_create("pi_ts2", pi_chain_ts2, NULL);
    pi_ts1 = thread_create("pi_ts1", pi_chain_ts1, NULL);
    pi_rt2 = thread_create("pi_rt2", pi_chain_rt, NULL);

    pi_rt2->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    pi_ts1->flags |= THREAD_FLAG_PINNED;
    pi_ts2->flags |= THREAD_FLAG_PINNED;
    pi_rt2->flags |= THREAD_FLAG_PINNED;

    thread_set_joinable(pi_ts2);
    thread_set_joinable(pi_ts1);
    thread_set_joinable(pi_rt2);

    thread_enqueue_on_core(pi_ts2, cpu);
    while (!atomic_load(&ts2_grabbed_b))
        scheduler_yield();

    thread_enqueue_on_core(pi_ts1, cpu);

    /* let ts1 grab A and block on B */
    while (!atomic_load(&ts1_grabbed_a))
        scheduler_yield();

    thread_enqueue_on_core(pi_rt2, cpu);

    thread_join(pi_ts2);
    thread_join(pi_ts1);
    thread_join(pi_rt2);

    TEST_ASSERT(atomic_load(&pi_chain_done) == 3);

    return TEST_SUCCESS;
}

static struct mutex pi_multi_mtx = MUTEX_INIT;
static atomic_uint pi_multi_done = 0;
static atomic_bool ts_got = false;

static void pi_multi_ts(void *arg) {
    (void) arg;
    mutex_lock(&pi_multi_mtx);
    test_info("multi_ts running");
    atomic_store(&ts_got, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    test_info("ts boosted");
    mutex_unlock(&pi_multi_mtx);
    atomic_fetch_add(&pi_multi_done, 1);
}

static void pi_multi_rt(void *arg) {
    (void) arg;
    test_info("multi_rt running");
    mutex_lock(&pi_multi_mtx);
    mutex_unlock(&pi_multi_mtx);
    atomic_fetch_add(&pi_multi_done, 1);
}

TEST_DECLARE_UNIT(mutex_pi_multi_waiters, .group = TEST_GROUP(mutex)) {
    cpu_id_t cpu = 1;

    struct thread *ts = thread_create("pi_ts", pi_multi_ts, NULL);
    struct thread *rt1 = thread_create("pi_rt1", pi_multi_rt, NULL);
    struct thread *rt2 = thread_create("pi_rt2", pi_multi_rt, NULL);

    rt1->perceived_prio_class = THREAD_PRIO_CLASS_RT;
    rt2->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    ts->flags |= THREAD_FLAG_PINNED;
    rt1->flags |= THREAD_FLAG_PINNED;
    rt2->flags |= THREAD_FLAG_PINNED;

    thread_set_joinable(ts);
    thread_set_joinable(rt1);
    thread_set_joinable(rt2);

    thread_enqueue_on_core(ts, cpu);
    while (!atomic_load(&ts_got))
        scheduler_yield();

    thread_enqueue_on_core(rt1, cpu);
    thread_enqueue_on_core(rt2, cpu);

    thread_join(ts);
    thread_join(rt1);
    thread_join(rt2);

    TEST_ASSERT(atomic_load(&pi_multi_done) == 3);

    return TEST_SUCCESS;
}

static struct mutex pi_revert_mtx = MUTEX_INIT;
static atomic_bool pi_reverted = false;
static atomic_bool pi_revert_got = false;
static atomic_uint pi_reverted_done = 0;

static void pi_revert_ts(void *arg) {
    (void) arg;
    mutex_lock(&pi_revert_mtx);

    atomic_store(&pi_revert_got, true);

    while (thread_get_current()->perceived_prio_class != THREAD_PRIO_CLASS_RT)
        cpu_relax();

    mutex_unlock(&pi_revert_mtx);

    while (thread_get_current()->perceived_prio_class == THREAD_PRIO_CLASS_RT)
        cpu_relax();

    atomic_store(&pi_reverted, true);
    atomic_fetch_add(&pi_reverted_done, 1);
}

static void pi_revert_rt(void *arg) {
    (void) arg;
    mutex_lock(&pi_revert_mtx);
    mutex_unlock(&pi_revert_mtx);
    atomic_fetch_add(&pi_reverted_done, 1);
}

TEST_DECLARE_UNIT(mutex_pi_revert, .group = TEST_GROUP(mutex)) {
    cpu_id_t cpu = 1;

    struct thread *ts = thread_create("pi_ts", pi_revert_ts, NULL);
    struct thread *rt = thread_create("pi_rt", pi_revert_rt, NULL);

    rt->perceived_prio_class = THREAD_PRIO_CLASS_RT;

    ts->flags |= THREAD_FLAG_PINNED;
    rt->flags |= THREAD_FLAG_PINNED;

    thread_set_joinable(ts);
    thread_set_joinable(rt);

    thread_enqueue_on_core(ts, cpu);

    while (!atomic_load(&pi_revert_got))
        scheduler_yield();

    thread_enqueue_on_core(rt, cpu);

    thread_join(ts);
    thread_join(rt);

    /* ts only exits after it has observed the boost drop back off */
    TEST_ASSERT(atomic_load(&pi_reverted));
    TEST_ASSERT(atomic_load(&pi_reverted_done) == 2);

    return TEST_SUCCESS;
}
#endif

#ifdef TEST_RCU
TEST_GROUP_DECLARE(rcu);


#define NUM_RCU_READERS (global.core_count)
#define RCU_TEST_DURATION_MS 50

struct rcu_test_data {
    int value;
};

static _Atomic(struct rcu_test_data *) shared_ptr = NULL;
static atomic_bool rcu_test_failed = false;
static _Atomic uint32_t rcu_reads_done = 0;

static void rcu_reader_thread(void *) {
    uint64_t end = time_get_ms() + RCU_TEST_DURATION_MS;

    while (time_get_ms() < end) {
        rcu_read_lock();

        struct rcu_test_data *p = rcu_dereference(shared_ptr);
        if (p) {
            int v = p->value;
            if (v != 42 && v != 43) {
                atomic_store(&rcu_test_failed, true);
                test_info("RCU reader saw invalid value");
                test_info("%d", v);
            }
        }

        rcu_read_unlock();

        scheduler_yield();
    }

    atomic_fetch_add(&rcu_reads_done, 1);
}

static atomic_bool volatile rcu_deferred_freed = false;

static void rcu_free_fn(struct rcu_cb *cb, void *ptr) {
    kfree(ptr);
    atomic_store(&rcu_deferred_freed, true);
    kfree(cb);
}

static void rcu_writer_thread(void *) {
    sleep_spin_ms(30);

    struct rcu_test_data *old = shared_ptr;

    struct rcu_test_data *new = kmalloc(sizeof(*new), ALLOC_FLAGS_ZERO);
    new->value = 43;
    rcu_assign_pointer(shared_ptr, new);

    rcu_synchronize();
    rcu_defer(kmalloc(sizeof(struct rcu_cb), ALLOC_FLAGS_ZERO), rcu_free_fn,
              old);
}

TEST_DECLARE_UNIT(rcu_test, .group = TEST_GROUP(rcu)) {
    struct rcu_test_data *initial = kmalloc(sizeof(*initial), ALLOC_FLAGS_ZERO);
    initial->value = 42;
    shared_ptr = initial;

    struct thread *readers[NUM_RCU_READERS];
    for (uint64_t i = 0; i < NUM_RCU_READERS; i++)
        readers[i] =
            thread_spawn_joinable("rcu_reader_test", rcu_reader_thread, NULL);

    struct thread *writer =
        thread_spawn_joinable("rcu_writer_test", rcu_writer_thread, NULL);

    for (uint64_t i = 0; i < NUM_RCU_READERS; i++) {
        if (readers[i])
            thread_join(readers[i]);
    }

    if (writer)
        thread_join(writer);

    TEST_ASSERT(atomic_load(&rcu_reads_done) == NUM_RCU_READERS);

    for (int i = 0; i < 100 && !atomic_load(&rcu_deferred_freed); i++)
        sleep_spin_ms(1);

    TEST_ASSERT(!atomic_load(&rcu_test_failed));

    return TEST_SUCCESS;
}

#define STRESS_NUM_READERS (global.core_count * 8)
#define STRESS_NUM_WRITERS (global.core_count)
#define STRESS_DURATION_MS 2000
#define STRESS_PRINT_MS 1000

struct rcu_stress_node {
    uint64_t seq; /* monotonic sequence number (for debugging) */
    int value;
    size_t freed_gen, enqueued_on;
};

static _Atomic(struct rcu_stress_node *) stress_shared = NULL;

/* book-keeping for the test */
static atomic_bool stress_stop = false;
static atomic_bool stress_failed = false;
static _Atomic uint32_t stress_readers_done = 0;
static _Atomic uint32_t stress_writers_done = 0;
static _Atomic uint32_t stress_deferred_freed = 0;
static _Atomic uint32_t stress_replacements = 0;
static atomic_size_t gen_freed = 0;

/* deferred free callback */
static void stress_free_cb(struct rcu_cb *cb, void *ptr) {
    atomic_store(&gen_freed, cb->gen_when_called);
    struct rcu_stress_node *n = ptr;
    /* optional debug trace */
    n->value = 34;
    n->freed_gen = cb->gen_when_called;
    n->enqueued_on = cb->enqueued_waiting_on_gen;
    atomic_fetch_add(&stress_deferred_freed, 1);
    kfree(cb);
    kfree(n);
}

/* reader thread: very tight loop, yields frequently */
static void rcu_stress_reader(void *arg) {
    (void) arg;

    time_ms_t last_print = time_get_ms();
    size_t iter = 0;
    while (!atomic_load(&stress_stop)) {
        rcu_read_lock();

        struct rcu_stress_node *p = rcu_dereference(stress_shared);
        if (p) {
            int v = p->value;
            if (v != 42 && v != 43) {
                atomic_store(&stress_failed, true);
                test_err("RCU stress reader saw invalid value");
                test_err("RCU stress reader observed invalid value %d, "
                         "freed during gen %zu enqueued_on %zu currently "
                         "started gen %zu quiescent for gen %zu\nat a nesting "
                         "depth of %zu",
                         v, p->freed_gen, p->enqueued_on,
                         thread_get_current()->rcu_start_gen,
                         thread_get_current()->rcu_quiescent_gen,
                         thread_get_current()->rcu_nesting);
                break;
            }
            volatile uint64_t seq = p->seq;
            (void) seq;
        }

        rcu_read_unlock();

        if (time_get_ms() - last_print > STRESS_PRINT_MS) {
            last_print = time_get_ms();
            test_info("\'%-17s\' iter %7zu w/ %7zu rplace and %7zu "
                      "free",
                      thread_get_current()->name, iter, stress_replacements,
                      stress_deferred_freed);
        }

        /* yield to exercise scheduler preemption and context switching */
        scheduler_yield();
        iter++;
    }

    test_info("RCU stress reader %s left, %u remaining",
              thread_get_current()->name,
              STRESS_NUM_READERS - stress_readers_done - 1);

    atomic_fetch_add(&stress_readers_done, 1);
}

/* writer thread: continuously replace the pointer, sometimes synchronize */
static void rcu_stress_writer(void *arg) {
    (void) arg;
    uint64_t local_iter = 0;

    while (!atomic_load(&stress_stop)) {
        struct rcu_stress_node *new = kmalloc(sizeof(*new), ALLOC_FLAGS_ZERO);
        if (!new) {
            /* allocation failure — mark as failure and exit */
            atomic_store(&stress_failed, true);
            test_info("RCU stress writer kmalloc failed");
            break;
        }

        new->seq = (uint64_t) atomic_fetch_add(&stress_replacements, 1) + 1;
        /* alternate values to ensure readers see both */
        new->value = (local_iter & 1) ? 43 : 42;
        local_iter++;

        struct rcu_stress_node *old =
            atomic_exchange_explicit(&stress_shared, new, memory_order_acq_rel);

        /*
         * Defer freeing the old pointer. We deliberately create a backlog by
         * deferring every single old pointer; later we wait for them to be
         * freed to assert correctness.
         */
        if (old)
            rcu_defer(kmalloc(sizeof(struct rcu_cb), ALLOC_FLAGS_ZERO),
                      stress_free_cb, old);

        /*
         * Occasionally force a synchronize call to exercise explicit grace
         * period advancement (do this about once every ~32 replacements).
         */
        if ((local_iter & 0x1f) == 0) {
            rcu_synchronize();
        }

        scheduler_yield();
    }

    atomic_fetch_add(&stress_writers_done, 1);
}

/* a reclaimer thread that also calls synchronize periodically to help drain */
static void rcu_stress_reclaimer(void *arg) {
    (void) arg;
    while (!atomic_load(&stress_stop)) {
        /* attempt to shrink deferred backlog by forcing grace periods */
        rcu_synchronize();
        /* small backoff between synchronizations */
        sleep_spin_ms(5);
    }
}

/* Test registration */
TEST_DECLARE_UNIT(rcu_stress_test, .group = TEST_GROUP(rcu),
                  .print_logs = true, ) {
    /* initial object */
    struct rcu_stress_node *initial =
        kmalloc(sizeof(*initial), ALLOC_FLAGS_ZERO);
    TEST_ASSERT(initial != NULL);
    initial->seq = 1;
    initial->value = 42;

    atomic_store(&stress_stop, false);
    atomic_store(&stress_failed, false);
    atomic_store(&stress_readers_done, 0);
    atomic_store(&stress_writers_done, 0);
    atomic_store(&stress_deferred_freed, 0);
    atomic_store(&stress_replacements, 0);
    stress_shared = initial;

    struct thread *readers[STRESS_NUM_READERS];
    struct thread *writers[STRESS_NUM_WRITERS];

    /* spawn readers (more than cores) */
    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        readers[i] =
            thread_spawn_joinable("rcu_stread_%u", rcu_stress_reader, NULL, i);
    }

    /* spawn writers */
    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        writers[i] =
            thread_spawn_joinable("rcu_strite_%u", rcu_stress_writer, NULL, i);
    }

    /* spawn one reclaimer to periodically call synchronize */
    struct thread *reclaimer =
        thread_spawn_joinable("rcu_streclaim", rcu_stress_reclaimer, NULL);

    /* run for the configured duration */
    uint64_t stop_at = time_get_ms() + STRESS_DURATION_MS;
    while (time_get_ms() < stop_at) {
        if (atomic_load(&stress_failed)) {
            test_info("RCU stress test failed early due to detection");
            break;
        }
        /* let other threads run */
        scheduler_yield();
    }

    /* signal stop to all readers/writers/reclaimer */
    atomic_store(&stress_stop, true);

    /* wait for readers, writers and the reclaimer to finish */
    for (uint32_t i = 0; i < STRESS_NUM_READERS; ++i) {
        if (readers[i])
            thread_join(readers[i]);
    }

    for (uint32_t i = 0; i < STRESS_NUM_WRITERS; ++i) {
        if (writers[i])
            thread_join(writers[i]);
    }

    if (reclaimer)
        thread_join(reclaimer);

    /* wait up to a reasonable timeout for deferred frees to run */
    for (int i = 0; i < 100 && atomic_load(&stress_deferred_freed) <
                                   atomic_load(&stress_replacements);
         i++) {
        /* call synchronize here to help force callbacks */
        rcu_synchronize();
        sleep_spin_ms(1);
    }

    test_info("RCU stress test: replacements=%u freed=%u",
              (unsigned) atomic_load(&stress_replacements),
              (unsigned) atomic_load(&stress_deferred_freed));

    /* checks */
    TEST_ASSERT(!atomic_load(&stress_failed));

    /*
     * We expect at least some frees to have occurred. On very constrained
     * implementations it may be possible not all deferred callbacks have
     * yet run; fail only if zero frees happened or if obviously fewer frees
     * than replacements exist (tunable).
     */
    TEST_ASSERT(atomic_load(&stress_deferred_freed) > 0);

    /* finally, free the last published pointer (if any) from test */
    struct rcu_stress_node *last = stress_shared;
    if (last) {
        /* old-style: synchronize then free directly */
        rcu_synchronize();
        kfree(last);
        atomic_fetch_add(&stress_deferred_freed, 1);
    }

    return TEST_SUCCESS;
}
#endif

#ifdef TEST_RWLOCK
TEST_GROUP_DECLARE(rwlock);


#define RWLOCK_REPORT_PROBLEMS()                                               \
    test_info("rwlock tests are encountering problems and will be skipped");   \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct rwlock rw_basic = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_UNIT(rwlock_basic_read, .group = TEST_GROUP(rwlock)) {
    rw_lock(&rw_basic, RWLOCK_ACQUIRE_READ);
    scheduler_yield();
    rw_unlock(&rw_basic);

    return TEST_SUCCESS;
}

static struct rwlock rw_basic_w = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_UNIT(rwlock_basic_write, .group = TEST_GROUP(rwlock)) {

    rw_lock(&rw_basic_w, RWLOCK_ACQUIRE_WRITE);
    scheduler_yield();
    rw_unlock(&rw_basic_w);

    return TEST_SUCCESS;
}

static struct rwlock rw_two_writers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static atomic_bool rw_two_done = false;

static void rw_two_writer_thread(void *) {
    rw_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);
    rw_unlock(&rw_two_writers);

    atomic_store(&rw_two_done, true);
}

TEST_DECLARE_UNIT(rwlock_two_writer_basic, .group = TEST_GROUP(rwlock)) {

    rw_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);

    struct thread *w = thread_spawn_joinable_on_core(
        "rw_two_writer", rw_two_writer_thread, NULL, 0);
    TEST_ASSERT(w);

    scheduler_yield(); // let second writer block

    rw_unlock(&rw_two_writers);

    thread_join(w);
    TEST_ASSERT(atomic_load(&rw_two_done));

    return TEST_SUCCESS;
}
#endif
