#include "../test_internal.h"

#ifdef TEST_MEM
TEST_DECLARE_UNIT(pmm_stress_alloc_free_test, .group = TEST_GROUP(mem),
                  TEST_INTENSITY(256, 2048, 32768)) {
    ABORT_IF_RAM_LOW();

    size_t iters = ctx->intensity_val ? ctx->intensity_val : 2048;
    paddr_t *pmm_stress_test_ptrs = kmalloc(sizeof(paddr_t) * iters);
    TEST_ASSERT(pmm_stress_test_ptrs != NULL);

    for (size_t i = 0; i < iters; i++) {
        pmm_stress_test_ptrs[i] = pmm_alloc_page();
        TEST_ASSERT(pmm_stress_test_ptrs[i] != 0);
    }

    for (int64_t i = (int64_t) iters - 1; i >= 0; i--) {
        pmm_free_page(pmm_stress_test_ptrs[i]);
    }

    kfree(pmm_stress_test_ptrs);
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

TEST_DECLARE_INTEGRATION(tlb_shootdown_synchronous_test,
                         .group = TEST_GROUP(mem),
                         TEST_INTENSITY_CORES(1, 1, 2, "threads/core")) {
    ABORT_IF_RAM_LOW();

    size_t nthreads = ctx->intensity_val ? ctx->intensity_val : 4;
    if (nthreads > TLB_MAX_TEST_THREADS)
        nthreads = TLB_MAX_TEST_THREADS;

    atomic_store(&tlb_go, false);
    atomic_store(&tlb_threads_done, 0);

    paddr_t p1 = pmm_alloc_page();
    paddr_t p2 = pmm_alloc_page();
    TEST_ASSERT(p1 && p2);

    void *va = vmm_map_bump(p1, PAGE_SIZE, 0);
    TEST_ASSERT(va);

    *(volatile uint64_t *) va = 0xAAAAAAAA;

    struct thread *t[TLB_MAX_TEST_THREADS];
    for (size_t i = 0; i < nthreads; i++) {
        t[i] = thread_spawn_joinable("tlb_reader", tlb_reader, (void *) i);
        TEST_ASSERT(t[i]);
        t[i]->private = va;
    }

    vmm_unmap_virt(va, PAGE_SIZE, VMM_FLAG_NONE);
    vmm_map_page((vaddr_t) va, p2, PAGE_WRITE);
    *(volatile uint64_t *) va = 0xBBBBBBBB;

    atomic_store(&tlb_go, true);
    tlb_shootdown((uintptr_t) va, true);

    for (size_t i = 0; i < nthreads; i++)
        thread_join(t[i]);

    for (size_t i = 0; i < nthreads; i++) {
        TEST_ASSERT(tlb_seen[i] == 0xBBBBBBBB);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_INTEGRATION(tlb_shootdown_async_eventual_test,
                         .group = TEST_GROUP(mem)) {
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

TEST_DECLARE_INTEGRATION(tlb_shootdown_flush_all_test, .group = TEST_GROUP(mem),
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

    TEST_ASSERT(*(volatile uint64_t *) va == 0xDEADBEEF);
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

TEST_DECLARE_INTEGRATION(tlb_shootdown_contention_test,
                         .group = TEST_GROUP(mem),
                         TEST_INTENSITY_CORES(1, 1, 2, "threads/core")) {
    size_t nthreads = ctx->intensity_val ? ctx->intensity_val : 4;
    if (nthreads > TLB_CONTENTION_MAX_THREADS)
        nthreads = TLB_CONTENTION_MAX_THREADS;

    struct thread *t[TLB_CONTENTION_MAX_THREADS];
    for (size_t i = 0; i < nthreads; i++) {
        t[i] = thread_spawn_joinable("tlb_spammer", tlb_spammer, NULL);
        TEST_ASSERT(t[i]);
    }

    for (size_t i = 0; i < nthreads; i++)
        thread_join(t[i]);

    test_info("concurrent shootdown stress completed");
    return TEST_SUCCESS;
}

TEST_DECLARE_SMOKE(page_alloc_demand_test, .group = TEST_GROUP(mem)) {
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

    /* touch every page of every buffer */
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

/* every page was faulted in by workers, so all frames are present */
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

/* Spawn nthreads workers over shared buffer set. single_core pins them,
 * and we can test all on one CPU vs spread out */
static void dp_spawn(struct thread **t, size_t nthreads, struct dp_worker *w,
                     bool single_core) {
    for (size_t i = 0; i < nthreads; i++) {
        uint64_t core = single_core ? 0 : (i % global.core_count);
        /* Join reference is what makes thread_pin safe, worker may
         * have already exited by then */
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

/* 1 buffer, N threads, 1 CPU = serialized faults + preemption mid-handler */
TEST_DECLARE_UNIT(demand_1buf_Nthreads_1cpu_test, .group = TEST_GROUP(mem),
                  TEST_INTENSITY(2, 8, 32)) {
    ABORT_IF_RAM_LOW();

    size_t nthreads = ctx->intensity_val ? ctx->intensity_val : 8;
    if (nthreads > DP_MAX_THREADS)
        nthreads = DP_MAX_THREADS;
    const size_t pages = DP_PAGES, nbuf = 1;
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

/* 1 buffer, N threads, N CPUs = many CPUs racing for same PTEs */
TEST_DECLARE_INTEGRATION(demand_1buf_Nthreads_Ncpu_test,
                         .group = TEST_GROUP(mem),
                         TEST_INTENSITY_CORES(1, 1, 4, "threads/core")) {
    ABORT_IF_RAM_LOW();

    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    const size_t pages = DP_PAGES, nbuf = 1;
    size_t nthreads =
        ctx->intensity_val ? ctx->intensity_val : global.core_count;
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

/* N buffers, M threads (M > N), N CPUs = contention spread over multiple
 * regions */
TEST_DECLARE_INTEGRATION(demand_Nbuf_Mthreads_Ncpu_test,
                         .group = TEST_GROUP(mem),
                         TEST_INTENSITY_CORES(1, 2, 4, "threads/core")) {
    ABORT_IF_RAM_LOW();

    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    const size_t pages = DP_PAGES;
    size_t nbuf = global.core_count;
    if (nbuf > DP_MAX_BUFS)
        nbuf = DP_MAX_BUFS;
    size_t nthreads =
        ctx->intensity_val ? ctx->intensity_val : (2 * nbuf); /* M > N */
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
#endif
