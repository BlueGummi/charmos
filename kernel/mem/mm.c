#include <asm.h>
#include <errno.h>
#include <kassert.h>
#include <math/align.h>
#include <mem/anon_vma.h>
#include <mem/fixed_size_alloc.h>
#include <mem/mm.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <smp/perdomain.h>
#include <structures/rbt.h>
#include <types/refcount.h>

#define MM_USER_MIN 0x0000000000010000UL
#define MM_USER_MAX 0x0000800000000000UL

FIXED_SIZE_RANGE_PERDOMAIN_DECLARE(mm, .obj_size = sizeof(struct mm),
                                   .obj_align = _Alignof(struct mm));

/* VMA tree is rbit keyed [vma_range_start, vma_range_end - 1], carrying custom
 * agumentation to track max_gap to make gap search O(log n)
 *
 * Non overlapping VMAs make 'left subtree max end' exactly the
 * predecessor's end, so no neighbor ptr needed here */
static size_t vma_range_node_min_low(struct rbit_node *n) {
    return n ? rbit_entry(n, struct vma_range, mm_node)->min_low : 0;
}

static size_t vma_range_node_gap(struct rbit_node *n) {
    return n ? rbit_entry(n, struct vma_range, mm_node)->max_gap : 0;
}

static bool vma_range_tree_augment(struct rbit_node *n) {
    struct vma_range *v = rbit_entry(n, struct vma_range, mm_node);
    size_t old_max = n->max, old_min = v->min_low, old_gap = v->max_gap;

    /* node->max: subtree max of interval.high (= max end - 1) */
    size_t mx = n->interval.high;
    if (rbit_node_max(n->left) > mx)
        mx = rbit_node_max(n->left);

    if (rbit_node_max(n->right) > mx)
        mx = rbit_node_max(n->right);

    size_t mn = n->left ? vma_range_node_min_low(n->left) : n->interval.low;

    /* max_gap: biggest free run between consecutive VMAs in the subtree */
    size_t gap = 0;
    if (n->left) {
        if (vma_range_node_gap(n->left) > gap)
            gap = vma_range_node_gap(n->left);

        size_t pred_end = rbit_node_max(n->left) + 1; /* predecessor's end */
        if (n->interval.low > pred_end && n->interval.low - pred_end > gap)
            gap = n->interval.low - pred_end;
    }
    if (n->right) {
        if (vma_range_node_gap(n->right) > gap)
            gap = vma_range_node_gap(n->right);

        size_t end = n->interval.high + 1; /* this VMA's end */
        size_t succ_low = vma_range_node_min_low(n->right);
        if (succ_low > end && succ_low - end > gap)
            gap = succ_low - end;
    }

    n->max = mx;
    v->min_low = mn;
    v->max_gap = gap;
    return mx != old_max || mn != old_min || gap != old_gap;
}

enum errno mm_pgtable_init(struct mm *mm) {
    mm->pml4 = vmm_make_user_pml4();
    return mm->pml4 ? ERR_OK : ERR_NO_MEM;
}

void mm_pgtable_free(struct mm *mm) {
    (void) mm;
    vmm_unmap_all_user_pages(vmm_phys_to_pml4(mm->pml4), VMM_FLAG_NONE);
}

void mm_activate(struct mm *mm) {
    write_cr3(mm->pml4);
}

enum errno mm_map_page(struct mm *mm, vaddr_t va, paddr_t pa, uint64_t pflags) {
    vmm_map_page_user(vmm_phys_to_pml4(mm->pml4), va, pa, pflags,
                      VMM_FLAG_USER);
    return ERR_OK;
}

struct mm *mm_alloc(void) {
    struct mm *mm = FSR_PERDOMAIN_ALLOC(mm);
    if (!mm)
        return NULL;

    rbit_init(&mm->vma_range_tree);
    mm->vma_range_tree.augment =
        vma_range_tree_augment; /* O(log n) gap search */
    /* Anyone can be under this rwlock */
    rwlock_init(&mm->lock, THREAD_PRIO_CLASS_URGENT);
    mm->vas = NULL;
    mm->mmap_cursor = MM_USER_MIN;

    refcount_init(&mm->users, 1);
    refcount_init(&mm->refcount, 1);

    if (mm_pgtable_init(mm) != ERR_OK) {
        FSR_PERDOMAIN_FREE(mm, mm);
        return NULL;
    }
    return mm;
}

void mm_free(struct mm *mm) {
    kassert(rbit_empty(&mm->vma_range_tree));
    mm_pgtable_free(mm);
    FSR_PERDOMAIN_FREE(mm, mm);
}

void mm_exit(struct mm *mm) {
    struct rbit_node *n = rbit_first(&mm->vma_range_tree);
    while (n) {
        struct rbit_node *next = rbit_next(n);
        struct vma_range *vma_range = rbit_entry(n, struct vma_range, mm_node);

        mm_vma_range_remove(mm, vma_range);
        vma_range_unlink_anon_vmas(vma_range); /* drops AVCs + anon_vma refs */
        /* TODO: drop this VMA's PTEs + folio rmap once page
         * tables can be torn down. For now the mappings die with the pml4 */
        vma_range_free(vma_range);

        n = next;
    }
}

struct vma_range *mm_vma_range_find(struct mm *mm, vaddr_t addr) {
    struct rbit_node *node = mm->vma_range_tree.root;
    struct vma_range *result = NULL;

    while (node) {
        struct vma_range *v = rbit_entry(node, struct vma_range, mm_node);
        if (vma_range_end(v) > addr) {
            result = v; /* candidate; look left for an earlier one */
            node = node->left;
        } else {
            node = node->right;
        }
    }
    return result;
}

struct vma_range *mm_vma_range_find_intersection(struct mm *mm, vaddr_t s,
                                                 vaddr_t e) {
    /* Inclusive interval [s, e-1] matches the half-open [s, e) reservation */
    struct interval iv = {.low = s, .high = e - 1};
    struct rbit_node *n = rbit_overlap_search(mm->vma_range_tree.root, iv);
    return n ? rbit_entry(n, struct vma_range, mm_node) : NULL;
}

void mm_vma_range_insert(struct mm *mm, struct vma_range *vma_range) {
    /* vma_range_init() already set the interval; rbit_insert() does the rest */
    kassert(!mm_vma_range_find_intersection(mm, vma_range_start(vma_range),
                                            vma_range_end(vma_range)));
    rbit_insert(&mm->vma_range_tree, &vma_range->mm_node);
}

void mm_vma_range_remove(struct mm *mm, struct vma_range *vma_range) {
    rbit_delete(&mm->vma_range_tree, &vma_range->mm_node);
}

struct gap_ctx {
    size_t len;
    size_t align;
    vaddr_t high;
};

static vaddr_t gap_fits(vaddr_t floor, vaddr_t ceil, const struct gap_ctx *c) {
    if (ceil <= floor)
        return 0;
    vaddr_t a = ALIGN_UP(floor, c->align);
    if (a < floor) /* ALIGN_UP wrapped */
        return 0;
    if (a + c->len < a) /* len overflow */
        return 0;
    if (a + c->len > ceil || a + c->len > c->high)
        return 0;
    return a;
}

static vaddr_t gap_search(struct rbit_node *n, vaddr_t *floor,
                          const struct gap_ctx *c) {
    if (!n)
        return 0;
    struct vma_range *v = rbit_entry(n, struct vma_range, mm_node);

    if (n->left) {
        if (vma_range_node_gap(n->left) >= c->len ||
            vma_range_node_min_low(n->left) > *floor) {
            vaddr_t r = gap_search(n->left, floor, c);
            if (r)
                return r;
        } else { /* prune: jump floor to the subtree's max end */
            vaddr_t e = rbit_node_max(n->left) + 1;
            if (e > *floor)
                *floor = e;
        }
    }

    /* gap right before this VMA */
    vaddr_t r = gap_fits(*floor, vma_range_start(v), c);
    if (r)
        return r;
    if (vma_range_start(v) >=
        c->high) /* this VMA and all to its right are out */
        return 0;
    if (vma_range_end(v) > *floor)
        *floor = vma_range_end(v);

    if (n->right) {
        if (vma_range_node_gap(n->right) >= c->len ||
            vma_range_node_min_low(n->right) > *floor) {
            vaddr_t rr = gap_search(n->right, floor, c);
            if (rr)
                return rr;
        } else {
            vaddr_t e = rbit_node_max(n->right) + 1;
            if (e > *floor)
                *floor = e;
        }
    }
    return 0;
}

vaddr_t mm_vma_range_find_gap(struct mm *mm, size_t len, size_t align,
                              vaddr_t low, vaddr_t high) {
    kassert(align != 0);
    if (len == 0 || high <= low || high - low < len)
        return 0;

    struct gap_ctx c = {.len = len, .align = align, .high = high};
    vaddr_t floor = low;

    vaddr_t r = gap_search(mm->vma_range_tree.root, &floor, &c);
    if (r)
        return r;

    /* trailing gap [floor, high) */
    return gap_fits(floor, high, &c);
}

vaddr_t mm_map(struct mm *mm, vaddr_t hint, size_t len,
               enum vma_range_protection prot, enum mm_map_flags flags) {
    len = PAGE_ALIGN_UP(len);
    if (len == 0)
        return 0;

    rw_write_lock(&mm->lock);

    vaddr_t addr;
    if (flags & MM_MAP_FIXED) {
        addr = PAGE_ALIGN_DOWN(hint);
        if (mm_vma_range_find_intersection(mm, addr, addr + len)) {
            /* TODO: clobber overlap via mm_unmap()
             * once it can tear down PTEs */

            /* FIXED map on busy range fails */
            rw_unlock(&mm->lock);
            return 0;
        }
    } else {
        vaddr_t low = hint ? PAGE_ALIGN_UP(hint) : mm->mmap_cursor;
        addr = mm_vma_range_find_gap(mm, len, PAGE_SIZE, low, MM_USER_MAX);
        if (!addr)
            addr = mm_vma_range_find_gap(mm, len, PAGE_SIZE, MM_USER_MIN,
                                         MM_USER_MAX);

        if (!addr) {
            rw_unlock(&mm->lock);
            return 0;
        }
        if (!hint) /* advance the cursor past what we just handed out */
            mm->mmap_cursor = addr + len;
    }

    /* anon reservation only, pages fault in lazily, so no PTEs */
    struct vma_range *vma_range = vma_range_alloc(mm, addr, addr + len, prot);
    if (!vma_range) {
        rw_unlock(&mm->lock);
        return 0;
    }
    mm_vma_range_insert(mm, vma_range);

    rw_unlock(&mm->lock);
    return addr;
}
