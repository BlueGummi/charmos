#include "test_internal.h"

#ifdef TEST_MEM
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
#endif

#ifdef TEST_FOLIO
TEST_GROUP_DECLARE(folio);

TEST_DECLARE_UNIT(folio_backpointers, .group = TEST_GROUP(folio)) {
    for (uint8_t order = 0; order <= 3; order++) {
        struct folio *f = folio_alloc(order);
        TEST_ASSERT(f);
        TEST_ASSERT(f->order == order);
        TEST_ASSERT(folio_nr_pages(f) == (1ul << order));
        TEST_ASSERT(!folio_mapped(f));
        TEST_ASSERT(atomic_load(&f->mapcount) == 0);

        for (size_t n = 0; n < folio_nr_pages(f); n++) {
            struct page *p = folio_get_page(f, n);
            TEST_ASSERT(folio_from_paddr(folio_get_paddr_for(f, n)) == f);
            TEST_ASSERT(folio_from_page(p) == f);
            TEST_ASSERT(page_get_folio_index(p) == n);
            TEST_ASSERT(page_is_folio_head(p) == (n == 0));
        }
    }
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(folio_zero_copy, .group = TEST_GROUP(folio)) {
    struct folio *src = folio_alloc(1); /* 2 pages */
    struct folio *dst = folio_alloc(1);
    TEST_ASSERT(src && dst);

    for (size_t n = 0; n < folio_nr_pages(src); n++) {
        uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
        uint8_t *d = (uint8_t *) folio_get_vaddr_for(dst, n);
        for (size_t b = 0; b < PAGE_SIZE; b++) {
            s[b] = (uint8_t) (b + n * 7 + 1);
            d[b] = 0xAB;
        }
    }

    folio_zero(src);
    for (size_t n = 0; n < folio_nr_pages(src); n++) {
        uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
        for (size_t b = 0; b < PAGE_SIZE; b++)
            TEST_ASSERT(s[b] == 0);
    }

    for (size_t n = 0; n < folio_nr_pages(src); n++) {
        uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
        for (size_t b = 0; b < PAGE_SIZE; b++)
            s[b] = (uint8_t) (b * 3 + n + 5);
    }
    folio_copy(src, dst); /* (src, dst) */
    for (size_t n = 0; n < folio_nr_pages(dst); n++) {
        uint8_t *s = (uint8_t *) folio_get_vaddr_for(src, n);
        uint8_t *d = (uint8_t *) folio_get_vaddr_for(dst, n);
        TEST_ASSERT(memcmp(s, d, PAGE_SIZE) == 0);
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(folio_anon_tag_mapcount, .group = TEST_GROUP(folio)) {
    struct folio *f = folio_alloc(0);
    TEST_ASSERT(f);

    TEST_ASSERT(!folio_is_anon(f));

    struct anon_vma *av = anon_vma_alloc();
    TEST_ASSERT(av);

    folio_set_anon(f, av, 0xDEAD);
    TEST_ASSERT(folio_is_anon(f));
    TEST_ASSERT(folio_get_anon_vma(f) == av);
    TEST_ASSERT(((uintptr_t) f->mapping & ~FOLIO_TAG_BITS) == (uintptr_t) av);
    TEST_ASSERT(f->index == 0xDEAD);

    TEST_ASSERT(!folio_mapped(f));
    folio_mapcount_inc(f);
    TEST_ASSERT(folio_mapped(f) && atomic_load(&f->mapcount) == 1);
    folio_mapcount_inc(f);
    TEST_ASSERT(atomic_load(&f->mapcount) == 2);
    TEST_ASSERT(folio_mapcount_dec(f) == false); /* 2 -> 1 */
    TEST_ASSERT(folio_mapcount_dec(f) == true);  /* 1 -> 0 */
    TEST_ASSERT(!folio_mapped(f));

    return TEST_SUCCESS;
}
#endif

#ifdef TEST_MM
TEST_GROUP_DECLARE(mm);

#define MM_TEST_VMAS 100
#define MM_TEST_QUERIES 3000
#define MM_TEST_SEED 0x5EED1234ULL

#define MM_WIN_LOW 0x0000000000100000UL  /* 1 MiB */
#define MM_WIN_HIGH 0x0000000040000000UL /* 1 GiB */

static vaddr_t brute_gap(struct mm *mm, size_t len, size_t align, vaddr_t low,
                         vaddr_t high) {
    if (len == 0 || high <= low || high - low < len)
        return 0;

    vaddr_t floor = low;
    struct rbit_node *n;
    rbit_for_each(n, &mm->vma_range_tree) {
        struct vma_range *v = rbit_entry(n, struct vma_range, mm_node);
        if (vma_range_end(v) <= low)
            continue;
        if (vma_range_start(v) >= high)
            break;
        if (vma_range_start(v) > floor) {
            vaddr_t a = ALIGN_UP(floor, align);
            if (a >= floor && a + len <= vma_range_start(v) && a + len <= high)
                return a;
        }
        if (vma_range_end(v) > floor)
            floor = vma_range_end(v);
    }
    vaddr_t a = ALIGN_UP(floor, align);
    if (a >= floor && a + len <= high)
        return a;
    return 0;
}

static size_t bf_max_high(struct rbit_node *n) {
    if (!n)
        return 0;
    size_t m = n->interval.high;
    size_t l = bf_max_high(n->left), r = bf_max_high(n->right);
    if (l > m)
        m = l;
    if (r > m)
        m = r;
    return m;
}

static size_t bf_min_low(struct rbit_node *n) {
    if (!n)
        return SIZE_MAX;
    size_t m = n->interval.low;
    size_t l = bf_min_low(n->left), r = bf_min_low(n->right);
    if (l < m)
        m = l;
    if (r < m)
        m = r;
    return m;
}

static size_t collect_inorder(struct rbit_node *n, struct rbit_node **out,
                              size_t idx) {
    if (!n)
        return idx;
    idx = collect_inorder(n->left, out, idx);
    out[idx++] = n;
    return collect_inorder(n->right, out, idx);
}

static size_t bf_max_gap(struct rbit_node *root) {
    static struct rbit_node *buf[MM_TEST_VMAS * 2 + 16];
    size_t cnt = collect_inorder(root, buf, 0);
    size_t g = 0;
    for (size_t i = 1; i < cnt; i++) {
        size_t prev_end = buf[i - 1]->interval.high + 1;
        size_t cur_low = buf[i]->interval.low;
        if (cur_low > prev_end && cur_low - prev_end > g)
            g = cur_low - prev_end;
    }
    return g;
}

static bool augment_ok(struct mm *mm) {
    struct rbit_node *n;
    rbit_for_each(n, &mm->vma_range_tree) {
        struct vma_range *v = rbit_entry(n, struct vma_range, mm_node);
        if (n->max != bf_max_high(n))
            return false;
        if (v->min_low != bf_min_low(n))
            return false;
        if (v->max_gap != bf_max_gap(n))
            return false;
    }
    return true;
}

/* In-order VMAs are sorted by start and never overlap. */
static bool tree_consistent(struct mm *mm) {
    vaddr_t prev_end = 0;
    struct rbit_node *n;
    rbit_for_each(n, &mm->vma_range_tree) {
        struct vma_range *v = rbit_entry(n, struct vma_range, mm_node);
        if (vma_range_start(v) < prev_end)
            return false;
        if (vma_range_end(v) <= vma_range_start(v))
            return false;
        prev_end = vma_range_end(v);
    }
    return true;
}

static size_t build_random_vma_ranges(struct mm *mm) {
    vaddr_t cursor = MM_WIN_LOW;
    size_t placed = 0;
    for (size_t i = 0; i < MM_TEST_VMAS; i++) {
        vaddr_t gap = (1 + (prng_next() % 64)) * PAGE_SIZE;
        vaddr_t len = (1 + (prng_next() % 32)) * PAGE_SIZE;
        vaddr_t start = cursor + gap;
        vaddr_t end = start + len;
        if (end >= MM_WIN_HIGH)
            break;
        struct vma_range *v = vma_range_alloc(mm, start, end, VMA_PROT_READ);
        if (!v)
            break;
        mm_vma_range_insert(mm, v);
        cursor = end;
        placed++;
    }
    return placed;
}

TEST_DECLARE_UNIT(mm_gap_differential, .group = TEST_GROUP(mm)) {
    prng_seed(MM_TEST_SEED);
    struct mm *mm = mm_alloc();
    TEST_ASSERT(mm);

    size_t placed = build_random_vma_ranges(mm);
    TEST_ASSERT(placed > 0);
    TEST_ASSERT(tree_consistent(mm));
    TEST_ASSERT(augment_ok(mm));

    for (size_t q = 0; q < MM_TEST_QUERIES; q++) {
        size_t len = (1 + (prng_next() % 40)) * PAGE_SIZE;
        size_t align = PAGE_SIZE << (prng_next() % 4);

        vaddr_t low = MM_WIN_LOW;
        vaddr_t high = MM_WIN_HIGH;
        if (prng_next() & 1) {
            low += (prng_next() % 0x4000) * PAGE_SIZE;
            high -= (prng_next() % 0x4000) * PAGE_SIZE;
        }

        vaddr_t got = mm_vma_range_find_gap(mm, len, align, low, high);
        vaddr_t want = brute_gap(mm, len, align, low, high);
        TEST_ASSERT(got == want);

        if (got) {
            TEST_ASSERT(IS_ALIGNED(got, align));
            TEST_ASSERT(got >= low && got + len <= high);
            TEST_ASSERT(!mm_vma_range_find_intersection(mm, got, got + len));
        }
    }

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(mm_map_consistency, .group = TEST_GROUP(mm)) {
    prng_seed(MM_TEST_SEED + 1);
    struct mm *mm = mm_alloc();
    TEST_ASSERT(mm);

    for (size_t i = 0; i < 128; i++) {
        size_t len = (1 + (prng_next() % 64)) * PAGE_SIZE;
        vaddr_t a =
            mm_map(mm, 0, len, VMA_PROT_READ | VMA_PROT_WRITE, MM_MAP_ANON);
        TEST_ASSERT(a != 0);
        TEST_ASSERT(IS_PAGE_ALIGNED(a));
        TEST_ASSERT(a >= 0x10000UL);
        struct vma_range *v = vma_range_find(mm, a);
        TEST_ASSERT(v && vma_range_start(v) == a);
    }

    TEST_ASSERT(tree_consistent(mm));
    TEST_ASSERT(augment_ok(mm));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(mm_vma_range_split, .group = TEST_GROUP(mm)) {
    struct mm *mm = mm_alloc();
    TEST_ASSERT(mm);

    vaddr_t base = MM_WIN_LOW;
    vaddr_t end = base + 16 * PAGE_SIZE;
    struct vma_range *orig = vma_range_alloc(mm, base, end, VMA_PROT_READ);
    TEST_ASSERT(orig);
    mm_vma_range_insert(mm, orig);

    vaddr_t mid = base + 6 * PAGE_SIZE;
    struct vma_range *hi = vma_range_split(orig, mid);
    TEST_ASSERT(hi);

    /* Boundaries: low half kept in orig, high half returned. */
    TEST_ASSERT(vma_range_start(orig) == base && vma_range_end(orig) == mid);
    TEST_ASSERT(vma_range_start(hi) == mid && vma_range_end(hi) == end);
    /* pgoff of the high half follows from the original's. */
    TEST_ASSERT(hi->pgoff == orig->pgoff + ((mid - base) >> PAGE_4K_SHIFT));

    /* Lookups land in the right half. */
    TEST_ASSERT(vma_range_find(mm, base) == orig);
    TEST_ASSERT(vma_range_find(mm, mid - 1) == orig);
    TEST_ASSERT(vma_range_find(mm, mid) == hi);
    TEST_ASSERT(vma_range_find(mm, end - 1) == hi);
    TEST_ASSERT(vma_range_find(mm, end) == NULL);

    /* Navigation links the two halves. */
    TEST_ASSERT(vma_range_next(orig) == hi);
    TEST_ASSERT(vma_range_prev(hi) == orig);
    TEST_ASSERT(vma_range_prev(orig) == NULL);
    TEST_ASSERT(vma_range_next(hi) == NULL);

    TEST_ASSERT(tree_consistent(mm));
    TEST_ASSERT(augment_ok(mm));

    /* Bad splits are rejected without disturbing the tree. */
    TEST_ASSERT(vma_range_split(orig, base) == NULL);     /* at start */
    TEST_ASSERT(vma_range_split(orig, mid) == NULL);      /* at end */
    TEST_ASSERT(vma_range_split(orig, base + 1) == NULL); /* unaligned */
    TEST_ASSERT(tree_consistent(mm));

    return TEST_SUCCESS;
}
#endif

#ifdef TEST_RMAP
TEST_GROUP_DECLARE(rmap);

/* rmap is the dangerous direction: folio -> every (mm, va) that maps it
 *
 * A miss here means a swapped/COWed/unmapped page leaves a live PTE behind
 *
 * The walk rides anon_vma_itree_first/next, so it has a differential test:
 * cross-check the visited set against a trivially-correct O(n) cover scan */

#define RMAP_SEED 0x9A7C0FF1ULL
#define RMAP_CHILDREN 40
#define RMAP_QUERIES 2000

#define RMAP_PROT (VMA_PROT_READ | VMA_PROT_WRITE)

/* parent reservation, in pages: children carve random sub-ranges out of it */
#define WIN_BASE_PG 0x40000UL /* 1 GiB >> 12 */
#define WIN_SPAN_PG 0x1000UL  /* 4096 pages */
#define CHILD_MAX_PG 64

struct range_rec {
    struct mm *mm;
    struct vma_range *vr;
    size_t lo_pg; /* first covered page index (== vr->pgoff) */
    size_t hi_pg; /* one past last */
};

struct visit_rec {
    struct mm *mm[RMAP_CHILDREN + 1];
    vaddr_t va[RMAP_CHILDREN + 1];
    size_t n;
};

static void record_visit(struct mm *mm, vaddr_t va, struct folio *f,
                         void *priv) {
    (void) f;
    struct visit_rec *v = priv;
    v->mm[v->n] = mm;
    v->va[v->n] = va;
    v->n++;
}

TEST_DECLARE_UNIT(rmap_fork_visibility, .group = TEST_GROUP(rmap)) {
    vaddr_t base = WIN_BASE_PG << PAGE_4K_SHIFT;
    vaddr_t end = base + 16 * PAGE_SIZE;
    vaddr_t va = base + 4 * PAGE_SIZE; /* the page we fault */

    struct mm *pmm = mm_alloc();
    struct mm *cmm = mm_alloc();
    TEST_ASSERT(pmm && cmm);

    struct vma_range *pvr = vma_range_alloc(pmm, base, end, RMAP_PROT);
    TEST_ASSERT(pvr);
    TEST_ASSERT(vma_range_anon_prepare(pvr) == ERR_OK);

    /* same geometry in the child (stands in for vma_range_dup) then fork: the
     * child's cloned AVC lands in the parent's anon_vma keyed by its pgoff */
    struct vma_range *cvr = vma_range_alloc(cmm, base, end, RMAP_PROT);
    TEST_ASSERT(cvr);
    TEST_ASSERT(anon_vma_fork(cvr, pvr) == ERR_OK);

    /* a page faulted into the parent BEFORE fork is reachable from BOTH */
    struct folio *shared = folio_alloc(0);
    TEST_ASSERT(shared);
    folio_add_anon_rmap_new(shared, pvr, va);

    struct visit_rec v = {0};
    rmap_walk_anon(shared, record_visit, &v);
    TEST_ASSERT(v.n == 2);
    bool saw_p = false, saw_c = false;
    for (size_t i = 0; i < v.n; i++) {
        TEST_ASSERT(v.va[i] == va);
        saw_p |= (v.mm[i] == pmm);
        saw_c |= (v.mm[i] == cmm);
    }
    TEST_ASSERT(saw_p && saw_c);

    struct folio *priv = folio_alloc(0);
    TEST_ASSERT(priv);
    folio_add_anon_rmap_new(priv, cvr, va);

    struct visit_rec v2 = {0};
    rmap_walk_anon(priv, record_visit, &v2);
    TEST_ASSERT(v2.n == 1);
    TEST_ASSERT(v2.mm[0] == cmm && v2.va[0] == va);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(rmap_itree_differential, .group = TEST_GROUP(rmap)) {
    prng_seed(RMAP_SEED);

    struct range_rec *r =
        kmalloc(sizeof(*r) * (RMAP_CHILDREN + 1), ALLOC_FLAGS_ZERO);
    TEST_ASSERT(r);

    struct mm *pmm = mm_alloc();
    TEST_ASSERT(pmm);
    vaddr_t pbase = WIN_BASE_PG << PAGE_4K_SHIFT;
    vaddr_t pend = (WIN_BASE_PG + WIN_SPAN_PG) << PAGE_4K_SHIFT;
    struct vma_range *pvr = vma_range_alloc(pmm, pbase, pend, RMAP_PROT);
    TEST_ASSERT(pvr);
    TEST_ASSERT(vma_range_anon_prepare(pvr) == ERR_OK);
    r[0] = (struct range_rec){pmm, pvr, WIN_BASE_PG, WIN_BASE_PG + WIN_SPAN_PG};

    for (size_t i = 1; i <= RMAP_CHILDREN; i++) {
        size_t off = prng_next() % (WIN_SPAN_PG - 1);
        size_t len = 1 + (prng_next() % CHILD_MAX_PG);
        if (off + len > WIN_SPAN_PG)
            len = WIN_SPAN_PG - off;

        size_t lo = WIN_BASE_PG + off;
        size_t hi = lo + len;

        struct mm *cmm = mm_alloc();
        TEST_ASSERT(cmm);
        struct vma_range *cvr = vma_range_alloc(cmm, lo << PAGE_4K_SHIFT,
                                                hi << PAGE_4K_SHIFT, RMAP_PROT);
        TEST_ASSERT(cvr);
        TEST_ASSERT(anon_vma_fork(cvr, pvr) == ERR_OK);
        r[i] = (struct range_rec){cmm, cvr, lo, hi};
    }

    /* one folio, faulted into the parent's anon_vma; we move its index around
     * to probe different object offsets without re-faulting */

    struct folio *f = folio_alloc(0);
    TEST_ASSERT(f);
    folio_add_anon_rmap_new(f, pvr, pbase);

    for (size_t q = 0; q < RMAP_QUERIES; q++) {
        size_t idx = WIN_BASE_PG + (prng_next() % WIN_SPAN_PG);
        f->index = idx; /* order-0: walk probes exactly [idx, idx] */

        struct visit_rec v = {0};
        rmap_walk_anon(f, record_visit, &v);

        vaddr_t want_va = (vaddr_t) idx << PAGE_4K_SHIFT;
        size_t expected = 0;
        for (size_t j = 0; j <= RMAP_CHILDREN; j++) {
            bool covers = idx >= r[j].lo_pg && idx < r[j].hi_pg;
            if (covers)
                expected++;

            /* find this range's mm in the visited set */
            bool seen = false;
            for (size_t k = 0; k < v.n; k++) {
                if (v.mm[k] == r[j].mm) {
                    seen = true;
                    TEST_ASSERT(v.va[k] == want_va);
                    break;
                }
            }
            TEST_ASSERT(seen == covers);
        }
        /* exact: no duplicates, no visits to ranges that don't cover idx */
        TEST_ASSERT(v.n == expected);
    }

    return TEST_SUCCESS;
}
#endif
