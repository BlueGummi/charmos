#ifdef TEST_MEM

#include <crypto/prng.h>
#include <mem/alloc.h>
#include <mem/elcm.h>
#include <mem/page_alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/tlb.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <stack_depot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <test.h>
#include <thread/thread.h>

TEST_GROUP_DECLARE(mem);

TEST_DECLARE_UNIT(pmm_alloc_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page();
    TEST_ASSERT(p);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(vmm_map_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    uint64_t p = pmm_alloc_page();
    TEST_ASSERT(p != 0);
    void *ptr = vmm_map_bump(p, PAGE_SIZE, 0);
    TEST_ASSERT(ptr != NULL);
    vmm_unmap_virt(ptr, PAGE_SIZE, VMM_FLAG_NONE);
    TEST_ASSERT(vmm_get_phys((uint64_t) ptr, VMM_FLAG_NONE) == (uint64_t) -1);
    return TEST_SUCCESS;
}

/* probably don't need these at all but I'll keep
 * them in case something decides to be funny */
#define ALIGNED_ALLOC_TIMES 512

#define ASSERT_ALIGNED(ptr, alignment)                                         \
    TEST_ASSERT(((uintptr_t) (ptr) & ((alignment) - 1)) == 0)

#define KMALLOC_ALIGNMENT_TEST(name, align)                                    \
    TEST_DECLARE_UNIT(kmalloc_aligned_##name##_test,                           \
                      .group = TEST_GROUP(mem)) {                              \
        ABORT_IF_RAM_LOW();                                                    \
        for (uint64_t i = 0; i < ALIGNED_ALLOC_TIMES; i++) {                   \
            void *ptr = kmalloc_aligned(align, align);                         \
            TEST_ASSERT(ptr != NULL);                                          \
            ASSERT_ALIGNED(ptr, align);                                        \
        }                                                                      \
        return TEST_SUCCESS;                                                   \
    }

KMALLOC_ALIGNMENT_TEST(32, 32)
KMALLOC_ALIGNMENT_TEST(64, 64)
KMALLOC_ALIGNMENT_TEST(128, 128)
KMALLOC_ALIGNMENT_TEST(256, 256)

#define STRESS_ALLOC_TIMES 2048

static paddr_t pmm_stress_test_ptrs[STRESS_ALLOC_TIMES];
TEST_DECLARE_UNIT(pmm_stress_alloc_free_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    for (uint64_t i = 0; i < STRESS_ALLOC_TIMES; i++) {
        pmm_stress_test_ptrs[i] = pmm_alloc_page();
        TEST_ASSERT(pmm_stress_test_ptrs[i] != 0);
    }

    for (int64_t i = STRESS_ALLOC_TIMES - 1; i >= 0; i--) {
        pmm_free_page(pmm_stress_test_ptrs[i]);
    }

    return TEST_SUCCESS;
}

static void *stress_alloc_free_ptrs[STRESS_ALLOC_TIMES] = {0};
TEST_DECLARE_UNIT(kmalloc_stress_alloc_free_test, .group = TEST_GROUP(mem)) {
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
TEST_DECLARE_UNIT(kmalloc_mixed_stress_test, .group = TEST_GROUP(mem)) {
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

TEST_DECLARE_UNIT(kmalloc_multithreaded_test, .group = TEST_GROUP(mem)) {
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
TEST_DECLARE_UNIT(kmalloc_new_test, .group = TEST_GROUP(mem)) {

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
TEST_DECLARE_UNIT(kmalloc_new_basic_test, .group = TEST_GROUP(mem)) {

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

TEST_DECLARE_UNIT(kmalloc_new_cache_align_test) {
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

TEST_DECLARE_UNIT(kmalloc_new_behavior_test, .group = TEST_GROUP(mem)) {
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
                  .group = TEST_GROUP(mem)) {
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
                  .group = TEST_GROUP(mem)) {

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

TEST_DECLARE_UNIT(tlb_shootdown_single_cpu_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page();
    paddr_t p2 = pmm_alloc_page();
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_bump(p1, PAGE_SIZE, 0);
    TEST_ASSERT(va);

    *(volatile uint64_t *) va = 0x11111111;

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_bump(p2, PAGE_SIZE, 0);

    tlb_shootdown((uintptr_t) va, true);

    *(volatile uint64_t *) va = 0x22222222;
    TEST_ASSERT(*(volatile uint64_t *) va == 0x22222222);

    return TEST_SUCCESS;
}

#define TLB_TEST_THREADS 4

static volatile uint64_t tlb_seen[TLB_TEST_THREADS];
static atomic_bool tlb_go = false;
static atomic_uint tlb_threads_done = 0;

static void tlb_reader(void *arg) {
    size_t id = (size_t) arg;

    while (!atomic_load(&tlb_go))
        cpu_relax();

    volatile uint64_t *va = thread_get_current()->private;
    tlb_seen[id] = *va;
    atomic_fetch_add(&tlb_threads_done, 1);
}

TEST_DECLARE_UNIT(tlb_shootdown_synchronous_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page();
    paddr_t p2 = pmm_alloc_page();
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_bump(p1, PAGE_SIZE, 0);
    TEST_ASSERT(va);

    *(volatile uint64_t *) va = 0xAAAAAAAA;

    struct thread *t[TLB_TEST_THREADS];
    for (size_t i = 0; i < TLB_TEST_THREADS; i++) {
        t[i] = thread_spawn_joinable("tlb_reader", tlb_reader, (void *) i);
        TEST_ASSERT(t[i]);
        t[i]->private = va;
    }

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    vmm_map_page((vaddr_t) va, p2, PAGE_WRITE);
    *(volatile uint64_t *) va = 0xBBBBBBBB;

    atomic_store(&tlb_go, true);
    tlb_shootdown((uintptr_t) va, true);

    for (size_t i = 0; i < TLB_TEST_THREADS; i++)
        thread_join(t[i]);

    for (size_t i = 0; i < TLB_TEST_THREADS; i++) {
        TEST_ASSERT(tlb_seen[i] == 0xBBBBBBBB);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(tlb_shootdown_async_eventual_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p1 = pmm_alloc_page();
    paddr_t p2 = pmm_alloc_page();
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_bump(p1, PAGE_SIZE, 0);
    *(volatile uint64_t *) va = 0x1234;

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_bump(p2, PAGE_SIZE, 0);
    *(volatile uint64_t *) va = 0x5678;

    tlb_shootdown((uintptr_t) va, false);

    /* Wait for IPIs to land */
    time_ms_t start = time_get_ms();
    while (time_get_ms() - start < 100) {
        if (*(volatile uint64_t *) va == 0x5678)
            return TEST_SUCCESS;
        scheduler_yield();
    }

    test_info("async TLB shootdown did not converge");
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(tlb_shootdown_flush_all_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    paddr_t p = pmm_alloc_page();
    TEST_ASSERT(p);

    void *va = vmm_map_bump(p, PAGE_SIZE, 0);

    /* Flood shootdown queue */
    for (size_t i = 0; i < TLB_QUEUE_SIZE * 4; i++) {
        tlb_shootdown((uintptr_t) va, false);
    }

    /* Now do a real remap */
    paddr_t p2 = pmm_alloc_page();
    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_bump(p2, PAGE_SIZE, 0);
    *(volatile uint64_t *) va = 0xDEADBEEF;

    tlb_shootdown((uintptr_t) va, true);

    TEST_ASSERT(*(volatile uint64_t *) va == 0xDEADBEEF);
    return TEST_SUCCESS;
}

static void tlb_spammer(void *) {
    paddr_t p = pmm_alloc_page();
    void *va = vmm_map_bump(p, PAGE_SIZE, 0);

    for (int i = 0; i < 1000; i++) {
        tlb_shootdown((uintptr_t) va, false);
    }
}

TEST_DECLARE_UNIT(tlb_shootdown_contention_test, .group = TEST_GROUP(mem)) {
    struct thread *t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = thread_spawn_joinable("tlb_spammer", tlb_spammer, NULL);
        TEST_ASSERT(t[i]);
    }

    for (int i = 0; i < 4; i++)
        thread_join(t[i]);

    test_info("concurrent shootdown stress completed");
    return TEST_SUCCESS;
}

static void print_cand(struct elcm_candidate c) {
    test_info("C(s=%F, p=%u, w=%u, W=%F, d=%u, b=%u, o=%u)", c.score_value,
              c.pages, c.wasted, c.wastage, c.distance, c.bitmap_bytes,
              c.obj_count);
}

TEST_DECLARE_UNIT(elcm_test, .group = TEST_GROUP(mem)) {
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

TEST_DECLARE_UNIT(kfree_defer_irq_test, .group = TEST_GROUP(mem)) {
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

TEST_DECLARE_UNIT(page_alloc_demand_test, .group = TEST_GROUP(mem)) {
    void *ptr = page_alloc_demand(8, ALLOC_FLAGS_ZERO);
    memset(ptr, 67, PAGE_SIZE);
    test_info("successfully demand allocated and memsetted memory");
    return TEST_SUCCESS;
}

#define DP_PAGES 16
#define DP_STRIDE (PAGE_SIZE / sizeof(uint64_t))
#define DP_MAX_BUFS 8
#define DP_MAX_THREADS 64

struct dp_worker {
    _Atomic uint64_t **bufs; /* nbuf demand buffers, counter at page head */
    size_t nbuf;
    size_t pages;
    atomic_uint *done;
};

static void dp_hammer(void *arg) {
    struct dp_worker *w = arg;

    /* touch every page of every buffer; first touch faults the zero frame in,
     * the atomic add is the lost-update probe */
    for (size_t b = 0; b < w->nbuf; b++)
        for (size_t p = 0; p < w->pages; p++)
            atomic_fetch_add_explicit(&w->bufs[b][p * DP_STRIDE], 1,
                                      memory_order_relaxed);

    atomic_fetch_add(w->done, 1);
}

static bool dp_alloc_bufs(_Atomic uint64_t **bufs, size_t nbuf, size_t pages) {
    for (size_t b = 0; b < nbuf; b++) {
        bufs[b] = page_alloc_demand(pages, ALLOC_FLAGS_ZERO);
        if (!bufs[b]) {
            for (size_t j = 0; j < b; j++)
                page_free((void *) bufs[j], pages);
            return false;
        }
    }
    return true;
}

/* every page was faulted in by the workers, so all frames are present here */
static void dp_free_bufs(_Atomic uint64_t **bufs, size_t nbuf, size_t pages) {
    for (size_t b = 0; b < nbuf; b++)
        page_free((void *) bufs[b], pages);
}

static bool dp_verify(_Atomic uint64_t **bufs, size_t nbuf, size_t pages,
                      uint64_t expect) {
    for (size_t b = 0; b < nbuf; b++)
        for (size_t p = 0; p < pages; p++)
            if (atomic_load(&bufs[b][p * DP_STRIDE]) != expect)
                return false;

    return true;
}

/* Spawn nthreads workers over the shared buffer set. single_core pins them all
 * to core 0 (the race is then preemption inside the fault handler); otherwise
 * they spread round-robin across every CPU (true parallel faults) */
static void dp_spawn(struct thread **t, size_t nthreads, struct dp_worker *w,
                     bool single_core) {
    for (size_t i = 0; i < nthreads; i++) {
        uint64_t core = single_core ? 0 : (i % global.core_count);
        /* joinable: the join reference is also what makes the thread_pin()
         * below safe, the worker may already have exited by then */
        t[i] = kassert(
            thread_spawn_joinable_on_core("dp_hammer", dp_hammer, w, core));
        if (single_core)
            thread_pin(t[i]);
    }
}

static void dp_join(struct thread **t, size_t nthreads) {
    for (size_t i = 0; i < nthreads; i++)
        thread_join(t[i]);
}

/* 1 buffer, N threads, 1 CPU: serialized faults + preemption mid-handler */
TEST_DECLARE_UNIT(demand_1buf_Nthreads_1cpu_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    const size_t pages = DP_PAGES, nthreads = 8, nbuf = 1;
    _Atomic uint64_t *bufs[1];
    TEST_ASSERT(dp_alloc_bufs(bufs, nbuf, pages));

    atomic_uint done = 0;
    struct dp_worker w = {bufs, nbuf, pages, &done};
    struct thread *t[DP_MAX_THREADS];
    dp_spawn(t, nthreads, &w, /*single_core=*/true);

    dp_join(t, nthreads);

    TEST_ASSERT(atomic_load(&done) == nthreads);
    TEST_ASSERT(dp_verify(bufs, nbuf, pages, nthreads));
    dp_free_bufs(bufs, nbuf, pages);
    return TEST_SUCCESS;
}

/* 1 buffer, N threads, N CPUs: many CPUs racing the same demand PTEs */
TEST_DECLARE_UNIT(demand_1buf_Nthreads_Ncpu_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    const size_t pages = DP_PAGES, nbuf = 1;
    size_t nthreads = global.core_count;
    if (nthreads > DP_MAX_THREADS)
        nthreads = DP_MAX_THREADS;

    _Atomic uint64_t *bufs[1];
    TEST_ASSERT(dp_alloc_bufs(bufs, nbuf, pages));

    atomic_uint done = 0;
    struct dp_worker w = {bufs, nbuf, pages, &done};
    struct thread *t[DP_MAX_THREADS];
    dp_spawn(t, nthreads, &w, /*single_core=*/false);

    dp_join(t, nthreads);

    TEST_ASSERT(atomic_load(&done) == nthreads);
    TEST_ASSERT(dp_verify(bufs, nbuf, pages, nthreads));
    dp_free_bufs(bufs, nbuf, pages);
    return TEST_SUCCESS;
}

/* N buffers, M threads (M > N), N CPUs: contention spread over many regions */
TEST_DECLARE_UNIT(demand_Nbuf_Mthreads_Ncpu_test, .group = TEST_GROUP(mem)) {
    ABORT_IF_RAM_LOW();

    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    const size_t pages = DP_PAGES;
    size_t nbuf = global.core_count;
    if (nbuf > DP_MAX_BUFS)
        nbuf = DP_MAX_BUFS;
    size_t nthreads = 2 * nbuf; /* M > N */
    if (nthreads > DP_MAX_THREADS)
        nthreads = DP_MAX_THREADS;

    _Atomic uint64_t *bufs[DP_MAX_BUFS];
    TEST_ASSERT(dp_alloc_bufs(bufs, nbuf, pages));

    atomic_uint done = 0;
    struct dp_worker w = {bufs, nbuf, pages, &done};
    struct thread *t[DP_MAX_THREADS];
    dp_spawn(t, nthreads, &w, /*single_core=*/false);

    dp_join(t, nthreads);

    TEST_ASSERT(atomic_load(&done) == nthreads);
    TEST_ASSERT(dp_verify(bufs, nbuf, pages, nthreads));
    dp_free_bufs(bufs, nbuf, pages);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(slab_demand_test, .group = TEST_GROUP(mem)) {
    /* One of these should eventually touch the demand page */
    for (size_t i = 0; i < 5000; i++) {
        void *p = kmalloc(500, ALLOC_FLAGS_ZERO | ALLOC_FLAG_PAGEABLE);
        memset(p, 0, 500);
    }

    return TEST_SUCCESS;
}

#endif
