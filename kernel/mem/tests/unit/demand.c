#include "mem/tests/test_internal.h"

#define DP_PAGES 16
#define DP_STRIDE (PAGE_SIZE / sizeof(uint64_t))
#define DP_MAX_BUFS ((size_t) 8)
#define DP_MAX_THREADS ((size_t) 64)

struct dp_worker {
    atomic_uint64_t **bufs; /* nbuf demand buffers, counter at page head */
    size_t nbuf;
    size_t pages;
    atomic_uint *done;
};

static void dp_hammer(void *arg) {
    struct dp_worker *w = arg;

    /* touch every page of every buffer */
    for (size_t b = 0; b < w->nbuf; b++)
        for (size_t p = 0; p < w->pages; p++)
            atomic_inc_rlx(&w->bufs[b][p * DP_STRIDE]);

    atomic_inc(w->done);
}

static bool dp_alloc_bufs(atomic_uint64_t **bufs, size_t nbuf, size_t pages) {
    for (size_t b = 0; b < nbuf; b++) {
        bufs[b] = page_alloc_demand(pages, ALLOC_ZERO);
        if (!bufs[b]) {
            for (size_t j = 0; j < b; j++)
                page_free((void *) bufs[j], pages);
            return false;
        }
    }
    return true;
}

/* every page was faulted in by workers, so all frames are present */
static void dp_free_bufs(atomic_uint64_t **bufs, size_t nbuf, size_t pages) {
    for (size_t b = 0; b < nbuf; b++)
        page_free((void *) bufs[b], pages);
}

static bool dp_verify(atomic_uint64_t **bufs, size_t nbuf, size_t pages,
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
        t[i] = kassert(thread_spawn(
            "dp_hammer", dp_hammer, .arg = w, .on_cpu = core, .joinable = true,
            .flags = single_core ? THREAD_FLAG_PINNED : 0));
    }
}

static void dp_join(struct thread **t, size_t nthreads) {
    for (size_t i = 0; i < nthreads; i++)
        thread_join(t[i]);
}

/* 1 buffer, N threads, 1 CPU = serialized faults + preemption mid-handler */
TEST_DEFINE_UNIT(mem, demand_single_buf, TEST_INTENSITY(2, 8, 32),
                 .min_ram_mib = 8) {
    size_t nthreads =
        MIN(ctx->intensity_val ? ctx->intensity_val : 8, DP_MAX_THREADS);
    const size_t pages = DP_PAGES, nbuf = 1;
    atomic_uint64_t *bufs[1];
    TEST_ASSERT(dp_alloc_bufs(bufs, nbuf, pages));

    atomic_uint done = 0;
    struct dp_worker w = {bufs, nbuf, pages, &done};
    struct thread *t[DP_MAX_THREADS];
    dp_spawn(t, nthreads, &w, /*single_core=*/true);

    dp_join(t, nthreads);

    TEST_ASSERT_EQ(atomic_load(&done), nthreads);
    TEST_ASSERT(dp_verify(bufs, nbuf, pages, nthreads));
    dp_free_bufs(bufs, nbuf, pages);
    return TEST_SUCCESS;
}
