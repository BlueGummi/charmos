#ifdef TEST_RMAP

#include <crypto/prng.h>
#include <errno.h>
#include <mem/alloc.h>
#include <mem/anon_vma.h>
#include <mem/folio.h>
#include <mem/mm.h>
#include <mem/rmap.h>
#include <mem/vma_range.h>
#include <stdint.h>
#include <test.h>

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

TEST_DECLARE(rmap_fork_visibility, .tier = TEST_TIER_UNIT) {
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

TEST_DECLARE(rmap_itree_differential, .tier = TEST_TIER_UNIT) {
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
