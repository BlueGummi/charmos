#include "mem/slab/tests/test_internal.h"

#ifdef TEST_MEM

#define MT_ALLOC_TIMES 1024

static atomic_int kmalloc_done = 0;

static void mt_kmalloc_worker(void *) {
    void *ptrs[MT_ALLOC_TIMES] = {0};

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        ptrs[i] = kmalloc(64);
        TEST_ASSERT_VOID_NONNULL(ptrs[i]);
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % MT_ALLOC_TIMES;

        if (ptrs[idx]) {
            kfree(ptrs[idx]);
            ptrs[idx] = NULL;
        }
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
        }
    }

    atomic_fetch_add(&kmalloc_done, 1);
}

TEST_DECLARE_INTEGRATION(slab, multithreaded_alloc_free,
                         TEST_INTENSITY_CORES(1, 2, 4, "threads/core")) {
    ABORT_IF_RAM_LOW();

    size_t nthreads = ctx->intensity_val ? ctx->intensity_val : 8;
    struct thread **threads = kmalloc(sizeof(struct thread *) * nthreads);
    TEST_ASSERT_NONNULL(threads);
    atomic_store(&kmalloc_done, 0);

    for (size_t i = 0; i < nthreads; i++) {
        threads[i] = thread_spawn_joinable_custom_stack(
            "mt_kmalloc_thread", mt_kmalloc_worker, NULL, PAGE_SIZE * 16);
        TEST_ASSERT_NONNULL(threads[i]);
    }

    for (size_t i = 0; i < nthreads; i++)
        thread_join(threads[i]);

    TEST_ASSERT_EQ(atomic_load(&kmalloc_done), (int) nthreads);
    kfree(threads);
    return TEST_SUCCESS;
}

#define STRESS_THREADS 7
#define STRESS_ITERS 50000
#define MAX_LIVE_ALLOCS 1024
#define SHOULD_FREE true

static atomic_bool all_ready = false;

struct stress_arg {
    int id;
    volatile int *done_flag;
    size_t iters;
};

static void stress_worker(void *) {
    struct stress_arg *a = NULL;
    /* wait until private field is visible */
    while (!(a = thread_get_current()->private))
        ;

    while (!all_ready)
        ;

    /* allocate small tracking table dynamically */
    void **live_ptrs = kmalloc(sizeof(void *) * MAX_LIVE_ALLOCS);
    memset(live_ptrs, 0, sizeof(void *) * MAX_LIVE_ALLOCS);

    for (size_t iter = 0; iter < a->iters; ++iter) {
        /* 1 in 8 chance to free something early (chaotic order) */
        if ((prng_next() & 7) == 0) {
            int idx = prng_next() % MAX_LIVE_ALLOCS;
            if (live_ptrs[idx]) {
                kfree_new(live_ptrs[idx], ALLOC_BEHAVIOR_NORMAL);
                live_ptrs[idx] = NULL;
            }
        }

        /* Allocate with randomized size and flags */
        size_t sz = 8 + (prng_next() % 512); /* small to moderate allocations */
        enum alloc_flags flags = ALLOC_FLAGS_DEFAULT;

        if (prng_next() & 1) {
            flags |= ALLOC_FLAG_PREFER_CACHE_ALIGNED;
            flags &= ~ALLOC_FLAG_NO_CACHE_ALIGN;
        }
        if (prng_next() & 2) {
            flags |= ALLOC_FLAG_NONMOVABLE;
            flags &= ~ALLOC_FLAG_MOVABLE;
        } else {
            flags |= ALLOC_FLAG_MOVABLE;
            flags &= ~ALLOC_FLAG_NONMOVABLE;
        }

        enum alloc_behavior behavior = (prng_next() & 3)
                                           ? ALLOC_BEHAVIOR_NORMAL
                                           : ALLOC_BEHAVIOR_NO_RECLAIM;

        void *p = kmalloc_new(sz, flags, behavior);
        if (!p)
            continue;

        /* write simple pattern to verify memory */
        ((uint8_t *) p)[0] = (uint8_t) (a->id + iter);
        ((uint8_t *) p)[sz - 1] = (uint8_t) (a->id ^ iter);

        /* randomly decide where to place it */
        int idx = prng_next() % MAX_LIVE_ALLOCS;

        if (live_ptrs[idx] && SHOULD_FREE)
            kfree_new(live_ptrs[idx], ALLOC_BEHAVIOR_NORMAL);
        live_ptrs[idx] = p;
    }

    /* Final cleanup */
    for (int i = 0; i < MAX_LIVE_ALLOCS; ++i) {
        if (live_ptrs[i])
            kfree_new(live_ptrs[i], ALLOC_BEHAVIOR_NORMAL);
    }

    kfree(live_ptrs);
    *a->done_flag = 1;
}

static volatile int done[STRESS_THREADS];
static struct stress_arg args[STRESS_THREADS];
static char msg[128];

TEST_DECLARE_INTEGRATION(slab, concurrency_stress,
                         TEST_INTENSITY(5000, 50000, 200000)) {
    memset((void *) done, 0, sizeof(done));
    all_ready = false;

    struct thread *workers[STRESS_THREADS];
    size_t iters = ctx->intensity_val ? ctx->intensity_val : 50000;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < STRESS_THREADS; ++i) {
        args[i].id = i;
        args[i].done_flag = &done[i];
        args[i].iters = iters;
        workers[i] = thread_spawn_joinable("kmalloc_new_stress_worker",
                                           stress_worker, NULL);

        workers[i]->private = &args[i];
    }
    irql_lower(irql);

    all_ready = true;

    /* the whole worker set shares one deadline */
    const time_ms_t timeout_ms = 30 * 1000;
    time_ms_t start = time_get_ms();

    for (int i = 0; i < STRESS_THREADS; ++i) {
        time_ms_t elapsed = time_get_ms() - start;
        time_ms_t left = elapsed >= timeout_ms ? 1 : timeout_ms - elapsed;

        if (!thread_join_timeout(workers[i], left, NULL)) {
            snprintf(msg, sizeof(msg), "thread %d did not complete in time", i);
            test_info(msg);

            /* still running, and we are done waiting on it */
            for (int j = i; j < STRESS_THREADS; ++j)
                thread_detach(workers[j]);

            return TEST_FAIL(msg);
        }

        if (!done[i]) {
            snprintf(msg, sizeof(msg), "thread %d exited without finishing", i);
            test_info(msg);

            for (int j = i + 1; j < STRESS_THREADS; ++j)
                thread_detach(workers[j]);

            return TEST_FAIL(msg);
        }
    }

    test_info("aggressive concurrency stress test completed");
    return TEST_SUCCESS;
}
#endif
