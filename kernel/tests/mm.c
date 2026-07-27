#ifdef TEST_MM

#include <crypto/prng.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/mm.h>
#include <mem/page.h>
#include <mem/vma_range.h>
#include <stdint.h>
#include <structures/rbit.h>
#include <test.h>

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

TEST_DECLARE(mm_gap_differential, .tier = TEST_TIER_UNIT,
             .group = TEST_GROUP(mm)) {
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

TEST_DECLARE(mm_map_consistency, .tier = TEST_TIER_UNIT,
             .group = TEST_GROUP(mm)) {
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

TEST_DECLARE(mm_vma_range_split, .tier = TEST_TIER_UNIT,
             .group = TEST_GROUP(mm)) {
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
