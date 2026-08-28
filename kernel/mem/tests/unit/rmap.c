#include "../test_internal.h"

#ifdef TEST_RMAP
TEST_GROUP_DECLARE(rmap, .intensity_desc = {
                             .curve = SCALE_PIECEWISE_LOG,
                             .unit = "queries",
                         });

/* rmap goes folio -> every (mm, va) mapping */

#define RMAP_SEED 0x9A7C0FF1ULL
#define RMAP_CHILDREN 40
#define RMAP_QUERIES 2000

#define RMAP_PROT (VMA_PROT_READ | VMA_PROT_WRITE)

/* parent reservation, in pages: children carve sub-ranges from it */
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

TEST_DECLARE_UNIT(rmap, rmap_fork_visibility) {
    vaddr_t base = WIN_BASE_PG << PAGE_4K_SHIFT;
    vaddr_t end = base + 16 * PAGE_SIZE;
    vaddr_t va = base + 4 * PAGE_SIZE; /* page we fault */

    struct mm *pmm = mm_alloc();
    struct mm *cmm = mm_alloc();
    TEST_ASSERT(pmm && cmm);

    struct vma_range *pvr = vma_range_alloc(pmm, base, end, RMAP_PROT);
    TEST_ASSERT_NONNULL(pvr);
    TEST_ASSERT_OK(vma_range_anon_prepare(pvr));

    /* same geometry in the child then fork, the child's cloned AVC should
     * be in parent's anon_vma, keyed by pgoff */
    struct vma_range *cvr = vma_range_alloc(cmm, base, end, RMAP_PROT);
    TEST_ASSERT_NONNULL(cvr);
    TEST_ASSERT_OK(anon_vma_fork(cvr, pvr));

    /* a page faulted into the parent BEFORE fork is reachable from BOTH */
    struct folio *shared = folio_alloc(0);
    TEST_ASSERT_NONNULL(shared);
    folio_add_anon_rmap_new(shared, pvr, va);

    struct visit_rec v = {0};
    rmap_walk_anon(shared, record_visit, &v);
    TEST_ASSERT_EQ(v.n, 2);
    bool saw_p = false, saw_c = false;
    for (size_t i = 0; i < v.n; i++) {
        TEST_ASSERT_EQ(v.va[i], va);
        saw_p |= (v.mm[i] == pmm);
        saw_c |= (v.mm[i] == cmm);
    }
    TEST_ASSERT(saw_p && saw_c);

    struct folio *priv = folio_alloc(0);
    TEST_ASSERT_NONNULL(priv);
    folio_add_anon_rmap_new(priv, cvr, va);

    struct visit_rec v2 = {0};
    rmap_walk_anon(priv, record_visit, &v2);
    TEST_ASSERT_EQ(v2.n, 1);
    TEST_ASSERT(v2.mm[0] == cmm && v2.va[0] == va);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(rmap, rmap_itree_differential,
                  TEST_INTENSITY(200, 2000, 10000)) {
    prng_seed(ctx->seed ? ctx->seed : RMAP_SEED);

    struct range_rec *r =
        kmalloc(sizeof(*r) * (RMAP_CHILDREN + 1), ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(r);

    struct mm *pmm = mm_alloc();
    TEST_ASSERT_NONNULL(pmm);
    vaddr_t pbase = WIN_BASE_PG << PAGE_4K_SHIFT;
    vaddr_t pend = (WIN_BASE_PG + WIN_SPAN_PG) << PAGE_4K_SHIFT;
    struct vma_range *pvr = vma_range_alloc(pmm, pbase, pend, RMAP_PROT);
    TEST_ASSERT_NONNULL(pvr);
    TEST_ASSERT_OK(vma_range_anon_prepare(pvr));
    r[0] = (struct range_rec){pmm, pvr, WIN_BASE_PG, WIN_BASE_PG + WIN_SPAN_PG};

    for (size_t i = 1; i <= RMAP_CHILDREN; i++) {
        size_t off = prng_next() % (WIN_SPAN_PG - 1);
        size_t len = 1 + (prng_next() % CHILD_MAX_PG);
        if (off + len > WIN_SPAN_PG)
            len = WIN_SPAN_PG - off;

        size_t lo = WIN_BASE_PG + off;
        size_t hi = lo + len;

        struct mm *cmm = mm_alloc();
        TEST_ASSERT_NONNULL(cmm);
        struct vma_range *cvr = vma_range_alloc(cmm, lo << PAGE_4K_SHIFT,
                                                hi << PAGE_4K_SHIFT, RMAP_PROT);
        TEST_ASSERT_NONNULL(cvr);
        TEST_ASSERT_OK(anon_vma_fork(cvr, pvr));
        r[i] = (struct range_rec){cmm, cvr, lo, hi};
    }

    /* one folio, faulted into parent's anon_vma, we move its index to
     * probe different object offsets without re-faulting */
    struct folio *f = folio_alloc(0);
    TEST_ASSERT_NONNULL(f);
    folio_add_anon_rmap_new(f, pvr, pbase);

    size_t queries = ctx->intensity_val ? ctx->intensity_val : RMAP_QUERIES;
    for (size_t q = 0; q < queries; q++) {
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

            /* find this range's mm in visited set */
            bool seen = false;
            for (size_t k = 0; k < v.n; k++) {
                if (v.mm[k] == r[j].mm) {
                    seen = true;
                    TEST_ASSERT_EQ(v.va[k], want_va);
                    break;
                }
            }
            TEST_ASSERT_EQ(seen, covers);
        }
        /* no duplicates, no visits to ranges that don't cover idx */
        TEST_ASSERT_EQ(v.n, expected);
    }

    kfree(r);
    return TEST_SUCCESS;
}
#endif
