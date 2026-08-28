#include "../test_internal.h"

#ifdef TEST_MEM

#define TLB_MAX_TEST_THREADS 64

static volatile uint64_t tlb_seen[TLB_MAX_TEST_THREADS];
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

TEST_DECLARE_INTEGRATION(mem, tlb_shootdown_synchronous_test,
                         TEST_INTENSITY_CORES(1, 1, 4, "threads/core")) {
    ABORT_IF_RAM_LOW();

    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    size_t nthreads =
        ctx->intensity_val ? ctx->intensity_val : global.core_count;
    if (nthreads > TLB_MAX_TEST_THREADS)
        nthreads = TLB_MAX_TEST_THREADS;

    atomic_store(&tlb_go, false);
    atomic_store(&tlb_threads_done, 0);

    paddr_t p1 = pmm_alloc_page();
    paddr_t p2 = pmm_alloc_page();
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_bump(p1, PAGE_SIZE, 0);
    *(volatile uint64_t *) va = 0xAAAAAAAAAAAAAAAA;

    struct thread *threads[TLB_MAX_TEST_THREADS];
    for (size_t i = 0; i < nthreads; i++) {
        threads[i] =
            thread_spawn_joinable("tlb_reader", tlb_reader, (void *) i);
        threads[i]->private = va;
    }

    /* Wait a tick so they spin on tlb_go */
    time_ms_t start = time_get_ms();
    while (time_get_ms() - start < 10)
        scheduler_yield();

    /* Remap under them */
    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    vmm_map_page((vaddr_t) va, p2, PAGE_PRESENT | PAGE_WRITE);
    *(volatile uint64_t *) va = 0xBBBBBBBBBBBBBBBB;

    /* Sync shootdown -- must complete on all cores before returning */
    tlb_shootdown((uintptr_t) va, true);

    /* Release workers to read */
    atomic_store(&tlb_go, true);

    for (size_t i = 0; i < nthreads; i++)
        thread_join(threads[i]);

    /* Every worker must have seen the new value, not old cached translation */
    for (size_t i = 0; i < nthreads; i++) {
        TEST_ASSERT_EQ(tlb_seen[i], 0xBBBBBBBBBBBBBBBB);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(mem, tlb_shootdown_async_eventual_test) {
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
    return TEST_FAIL("async TLB shootdown did not converge within timeout");
}

TEST_DECLARE_INTEGRATION(mem, tlb_shootdown_flush_all_test,
                         TEST_INTENSITY(64, 256, 4096)) {
    ABORT_IF_RAM_LOW();

    size_t iters =
        ctx->intensity_val ? ctx->intensity_val : (TLB_QUEUE_SIZE * 4);

    paddr_t p = pmm_alloc_page();
    TEST_ASSERT(p);

    void *va = vmm_map_bump(p, PAGE_SIZE, 0);

    /* Flood shootdown queue */
    for (size_t i = 0; i < iters; i++) {
        tlb_shootdown((uintptr_t) va, false);
    }

    /* Now do remap */
    paddr_t p2 = pmm_alloc_page();
    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    va = vmm_map_bump(p2, PAGE_SIZE, 0);
    *(volatile uint64_t *) va = 0xDEADBEEF;

    tlb_shootdown((uintptr_t) va, true);

    TEST_ASSERT_EQ(*(volatile uint64_t *) va, 0xDEADBEEF);
    return TEST_SUCCESS;
}

#define TLB_CONTENTION_MAX_THREADS 64

static void tlb_spammer(void *) {
    paddr_t p = pmm_alloc_page();
    void *va = vmm_map_bump(p, PAGE_SIZE, 0);

    for (int i = 0; i < 1000; i++) {
        tlb_shootdown((uintptr_t) va, false);
    }
}

TEST_DECLARE_INTEGRATION(mem, tlb_shootdown_contention_test,
                         TEST_INTENSITY_CORES(1, 1, 2, "threads/core")) {
    size_t nthreads = ctx->intensity_val ? ctx->intensity_val : 4;
    if (nthreads > TLB_CONTENTION_MAX_THREADS)
        nthreads = TLB_CONTENTION_MAX_THREADS;

    struct thread *t[TLB_CONTENTION_MAX_THREADS];
    for (size_t i = 0; i < nthreads; i++) {
        t[i] = thread_spawn_joinable("tlb_spammer", tlb_spammer, NULL);
        TEST_ASSERT_NONNULL(t[i]);
    }

    for (size_t i = 0; i < nthreads; i++)
        thread_join(t[i]);

    test_info("concurrent shootdown stress completed");
    return TEST_SUCCESS;
}
#endif
