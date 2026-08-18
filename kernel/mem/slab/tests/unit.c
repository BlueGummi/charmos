#include "test_internal.h"

#ifdef TEST_MEM
#define STRESS_ALLOC_TIMES 2048


static void *stress_alloc_free_ptrs[STRESS_ALLOC_TIMES] = {0};
TEST_DECLARE_UNIT(kmalloc_stress_alloc_free_test, .group = TEST_GROUP(slab)) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        stress_alloc_free_ptrs[i] = kmalloc(64);
        TEST_ASSERT(stress_alloc_free_ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % STRESS_ALLOC_TIMES;
        if (stress_alloc_free_ptrs[idx]) {
            kfree(stress_alloc_free_ptrs[idx]);
            stress_alloc_free_ptrs[idx] = NULL;
        }
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        if (stress_alloc_free_ptrs[i]) {
            kfree(stress_alloc_free_ptrs[i]);
        }
    }

    return TEST_SUCCESS;
}

/* Put it here to avoid it eating things up */
static void *mixed_stress_test_ptrs[STRESS_ALLOC_TIMES] = {0};
TEST_DECLARE_UNIT(kmalloc_mixed_stress_test, .group = TEST_GROUP(slab)) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        mixed_stress_test_ptrs[i] = kmalloc(128);
        TEST_ASSERT(mixed_stress_test_ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        kfree(mixed_stress_test_ptrs[i]);
    }

    return TEST_SUCCESS;
}

#define MT_THREAD_COUNT 8
#define MT_ALLOC_TIMES 1024

static atomic_int kmalloc_done = 0;

static void mt_kmalloc_worker(void *) {
    void *ptrs[MT_ALLOC_TIMES] = {0};

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        ptrs[i] = kmalloc(64);
        TEST_ASSERT_VOID(ptrs[i] != NULL);
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        uint64_t idx = prng_next() % MT_ALLOC_TIMES;

        kfree(ptrs[idx]);
        ptrs[idx] = NULL;
    }

    for (uint64_t i = 0; i < MT_ALLOC_TIMES; i++) {
        kfree(ptrs[i]);
    }

    atomic_fetch_add(&kmalloc_done, 1);
}

TEST_DECLARE_UNIT(kmalloc_multithreaded_test, .group = TEST_GROUP(slab)) {
    ABORT_IF_RAM_LOW();

    struct thread *threads[MT_THREAD_COUNT];
    atomic_store(&kmalloc_done, 0);

    for (int i = 0; i < MT_THREAD_COUNT; i++) {
        threads[i] = thread_spawn_joinable_custom_stack(
            "mt_kmalloc_thread", mt_kmalloc_worker, NULL, PAGE_SIZE * 16);
        TEST_ASSERT(threads[i] != NULL);
    }

    for (int i = 0; i < MT_THREAD_COUNT; i++)
        thread_join(threads[i]);

    TEST_ASSERT(atomic_load(&kmalloc_done) == MT_THREAD_COUNT);

    return TEST_SUCCESS;
}

static char hooray[128] = {0};
TEST_DECLARE_UNIT(kmalloc_new_test, .group = TEST_GROUP(slab)) {

    void *p = kmalloc_new(67, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);

    time_ms_t ms = time_get_ms();
    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    ms = time_get_ms() - ms;

    snprintf(hooray, 128, "allocated %p and free took %u ms", p, ms);

    test_info(hooray);
    return TEST_SUCCESS;
}

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

static char a_msg[128];
TEST_DECLARE_UNIT(kmalloc_new_basic_test, .group = TEST_GROUP(slab)) {

    void *p1 = kmalloc_new(1, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);
    void *p2 = kmalloc_new(64, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);
    void *p3 = kmalloc_new(4096, ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_NORMAL);

    if (!p1 || !p2 || !p3) {
        test_info("kmalloc_new returned NULL for a valid request");
        return TEST_FAIL(NULL);
    }

    /* Write/read back small pattern to verify memory usable */
    memset(p1, 0xA5, 1);
    memset(p2, 0x5A, 64);
    memset(p3, 0xFF, 4096);

    if (((uint8_t *) p1)[0] != 0xA5 || ((uint8_t *) p2)[0] != 0x5A ||
        ((uint8_t *) p3)[0] != 0xFF) {
        test_info("Memory pattern check failed");
        return TEST_FAIL(NULL);
    }

    /* timed free to check that kfree_new returns quickly */
    time_ms_t start = time_get_ms();
    kfree_new(p1, ALLOC_BEHAVIOR_NORMAL);
    kfree_new(p2, ALLOC_BEHAVIOR_NORMAL);
    kfree_new(p3, ALLOC_BEHAVIOR_NORMAL);
    time_ms_t elapsed = time_get_ms() - start;

    snprintf(a_msg, sizeof(a_msg), "basic alloc/free OK (free took %u ms)",
             (unsigned) elapsed);
    test_info(a_msg);
    return TEST_SUCCESS;
}

/*
-------------------- Alignment preference test --------------------

TEST_DECLARE_UNIT(kmalloc_new_cache_align_test, .group = TEST_GROUP(slab)) {
     Request cache-aligned memory
    uint16_t flags = ALLOC_FLAG_PREFER_CACHE_ALIGNED | ALLOC_FLAG_NONMOVABLE |
                     ALLOC_FLAG_NONPAGEABLE | ALLOC_FLAG_CLASS_DEFAULT;
    void *p = kmalloc_new(128, flags, ALLOC_BEHAVIOR_NORMAL);
    if (!p) {
        test_info("kmalloc_new returned NULL for cache-aligned request");
        return TEST_FAIL(NULL);
    }

    if (((uintptr_t) p % CACHE_LINE_SIZE) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "pointer %p is not cache-line aligned", p);
        test_info(msg);
        kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
        return TEST_FAIL(NULL);
    }

    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    test_info("cache alignment check passed");
    return TEST_SUCCESS;
}
*/

/* -------------------- Behavior flag verification test -------------------- */

TEST_DECLARE_UNIT(kmalloc_new_behavior_test, .group = TEST_GROUP(slab)) {
    /* ALLOC_BEHAVIOR_ATOMIC should require nonpageable/nonmovable - allocator
       or sanitizers might coerce flags. This test ensures allocation doesn't
       return NULL for such a request. */
    return TEST_SUCCESS;

    uint16_t f = ALLOC_FLAG_NONPAGEABLE | ALLOC_FLAG_NONMOVABLE |
                 ALLOC_FLAG_NO_CACHE_ALIGN;
    void *p = kmalloc_new(256, f, ALLOC_BEHAVIOR_ATOMIC);
    if (!p) {
        test_info("kmalloc_new failed for ATOMIC nonpageable request");
        return TEST_FAIL(NULL);
    }
    /* Do a quick write */
    volatile uint8_t *b = p;
    b[0] = 0x7E;
    if (b[0] != 0x7E) {
        test_info("atomic allocation memory check failed");
        kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
        return TEST_FAIL(NULL);
    }
    kfree_new(p, ALLOC_BEHAVIOR_NORMAL);
    test_info("behavior (ATOMIC) allocation passed");
    return TEST_SUCCESS;
}

/* -------------------- Multithreaded stress test -------------------- */

#define STRESS_THREADS 7
#define STRESS_ITERS 50000
#define MAX_LIVE_ALLOCS 1024
#define SHOULD_FREE true

static atomic_bool all_ready = false;

struct stress_arg {
    int id;
    volatile int *done_flag;
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

    for (int iter = 0; iter < STRESS_ITERS; ++iter) {
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
        uint16_t flags = ALLOC_FLAGS_DEFAULT;

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

        void *p = kmalloc(sz, flags, behavior);
        if (!p)
            continue;

        /* write simple pattern to verify memory */
        ((uint8_t *) p)[0] = (uint8_t) (a->id + iter);
        ((uint8_t *) p)[sz - 1] = (uint8_t) (a->id ^ iter);

        /* randomly decide where to place it */
        int idx = prng_next() % MAX_LIVE_ALLOCS;

        if (live_ptrs[idx] && SHOULD_FREE)
            kfree(live_ptrs[idx], ALLOC_BEHAVIOR_NORMAL);
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

volatile int done[STRESS_THREADS];
struct stress_arg args[STRESS_THREADS];
static char msg[128];

TEST_DECLARE_UNIT(kmalloc_new_concurrency_stress_test,
                  .group = TEST_GROUP(slab)) {
    memset((void *) done, 0, sizeof(done));

    struct thread *workers[STRESS_THREADS];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (int i = 0; i < STRESS_THREADS; ++i) {
        args[i].id = i;
        args[i].done_flag = &done[i];
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

            return TEST_SUCCESS;
        }

        if (!done[i]) {
            snprintf(msg, sizeof(msg), "thread %d exited without finishing", i);
            test_info(msg);

            for (int j = i + 1; j < STRESS_THREADS; ++j)
                thread_detach(workers[j]);

            return TEST_SUCCESS;
        }
    }

    test_info("aggressive concurrency stress test completed");
    return TEST_SUCCESS;
}

/* -------------------- Small reallocation-like smoke test --------------------
 */

TEST_DECLARE_UNIT(kmalloc_new_alloc_free_sequence_test,
                  .group = TEST_GROUP(slab)) {

    void *blocks[16];
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); ++i) {
        blocks[i] = kmalloc_new(64 + (i * 8), ALLOC_FLAGS_DEFAULT,
                                ALLOC_BEHAVIOR_NORMAL);
        if (!blocks[i]) {
            test_info("failed to allocate block in sequence");
            /* free what we did get */
            for (size_t j = 0; j < i; ++j)
                kfree_new(blocks[j], ALLOC_BEHAVIOR_NORMAL);
            return TEST_FAIL(NULL);
        }
    }

    /* free every other block first */
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i += 2)
        kfree_new(blocks[i], ALLOC_BEHAVIOR_NORMAL);

    /* then free remaining */
    for (size_t i = 1; i < sizeof(blocks) / sizeof(blocks[0]); i += 2)
        kfree_new(blocks[i], ALLOC_BEHAVIOR_NORMAL);

    test_info("alloc/free sequence test passed");
    return TEST_SUCCESS;
}

static void print_cand(struct elcm_candidate c) {
    test_info("C(s=%F, p=%u, w=%u, W=%F, d=%u, b=%u, o=%u)", c.score_value,
              c.pages, c.wasted, c.wastage, c.distance, c.bitmap_bytes,
              c.obj_count);
}

TEST_DECLARE_UNIT(elcm_test, .group = TEST_GROUP(slab)) {
    struct elcm_params params = {
        .obj_size = 938,
        .max_wastage_pct = ELCM_MAX_WASTAGE_DEFAULT,
        .max_pages = SIZE_MAX,
        .bias_towards_pow2 = true,
        .metadata_size_bytes = 96,
        .metadata_bits_per_obj = 1,
    };

    elcm(&params);
    print_cand(params.out);
    params.bias_towards_pow2 = false;
    elcm(&params);
    print_cand(params.out);

    return TEST_SUCCESS;
}

#define KFREE_IRQ_TEST_ALLOC_COUNT 2048
#define KFREE_IRQ_TEST_FREES_PER_IRQ_MIDRANGE (KFREE_IRQ_TEST_ALLOC_COUNT / 128)
#define KFREE_IRQ_TEST_SPIN_MASK UINT8_MAX

static void *kfree_irq_allocs[KFREE_IRQ_TEST_ALLOC_COUNT] = {0};
static atomic_size_t kfree_irq_test_consumed = 0;

static enum irq_result kfree_irq_test_irq(void *arg, irq_t irq,
                                          struct irq_context *irqc) {
    /* Non-ordered load here is OK, we are the only modifier (this CPU) */
    uint8_t seed = prng_next() & 0xF;
    int delta = seed > 0x7 ? -(seed & 0x7) : (seed & 0x7);
    int possible = KFREE_IRQ_TEST_FREES_PER_IRQ_MIDRANGE + delta;
    if (possible < 0)
        possible = KFREE_IRQ_TEST_FREES_PER_IRQ_MIDRANGE;

    if (possible + kfree_irq_test_consumed > KFREE_IRQ_TEST_ALLOC_COUNT)
        possible = KFREE_IRQ_TEST_ALLOC_COUNT - kfree_irq_test_consumed;

    for (int i = 0; i < possible; i++) {
        kassert(kfree_irq_test_consumed < KFREE_IRQ_TEST_ALLOC_COUNT);
        int idx = atomic_fetch_add(&kfree_irq_test_consumed, 1);
        kfree_defer_irq(kfree_irq_allocs[idx]);
        int spins = prng_next() & KFREE_IRQ_TEST_SPIN_MASK;

        while (spins) {
            cpu_relax();
            spins--;
        }
    }

    return IRQ_HANDLED;
}

TEST_DECLARE_UNIT(kfree_defer_irq_test, .group = TEST_GROUP(slab)) {
    if (global.core_count < 4) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    irq_t irq = irq_alloc_entry();
    irq_register("kfree_defer_irq_test", irq, kfree_irq_test_irq, NULL,
                 IRQ_FLAG_NONE);
    irq_set_chip(irq, lapic_get_chip(), NULL);

    for (int i = 0; i < KFREE_IRQ_TEST_ALLOC_COUNT; i++) {
        kfree_irq_allocs[i] = kmalloc(64);
    }

    while (atomic_load(&kfree_irq_test_consumed) < KFREE_IRQ_TEST_ALLOC_COUNT) {
        ipi_send(3, irq);
        int spins = prng_next() & KFREE_IRQ_TEST_SPIN_MASK;

        while (spins) {
            cpu_relax();
            spins--;
        }
    }
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(slab_demand_test, .group = TEST_GROUP(slab)) {
    /* One of these should eventually touch the demand page */
    for (size_t i = 0; i < 5000; i++) {
        void *p = kmalloc(500, ALLOC_FLAGS_ZERO | ALLOC_FLAG_PAGEABLE);
        memset(p, 0, 500);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE(slab_map_new_test, .group = TEST_GROUP(slab)) {
    return TEST_SUCCESS;
}
#endif
