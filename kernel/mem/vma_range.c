#include <enforce.h>
#include <err.h>
#include <mem/anon_vma.h>
#include <mem/avc.h>
#include <mem/fixed_size_alloc.h>
#include <mem/mm.h>
#include <mem/vma_range.h>
#include <smp/perdomain.h>
#include <structures/list.h>
#include <types/refcount.h>

FIXED_SIZE_RANGE_PERDOMAIN_DEFINE(vma_range,
                                  .obj_size = sizeof(struct vma_range),
                                  .obj_align = _Alignof(struct vma_range));

void vma_range_init(struct vma_range *vma_range, struct mm *mm, vaddr_t start,
                    vaddr_t end, enum vma_range_protection prot) {
    /* fold [start, end) into interval node */
    vma_range->mm_node.interval.low = start;
    vma_range->mm_node.interval.high = end - 1;

    /* convention: object space offset == virtual page index, so
     * vma_range_address() is an identity over [start, end) and makes pgoff
     * survive fork unchanged (child keeping same VA layout), the folio's
     * `index` resolves to the right VA in every mm that ends up mapping it */
    vma_range->pgoff = start >> PAGE_4K_SHIFT;
    vma_range->prot = prot;
    vma_range->anon_vma = NULL;
    vma_range->mm = mm;
    list_head_init(&vma_range->anon_vma_chain);
}

struct vma_range *vma_range_alloc(struct mm *mm, vaddr_t start, vaddr_t end,
                                  enum vma_range_protection prot) {
    struct vma_range *vma_range = FSR_PERDOMAIN_ALLOC(vma_range);
    if (!vma_range)
        return NULL;

    vma_range_init(vma_range, mm, start, end, prot);
    return vma_range;
}

void vma_range_free(struct vma_range *vma_range) {
    enforce(list_head_empty(&vma_range->anon_vma_chain));
    FSR_PERDOMAIN_FREE(vma_range, vma_range);
}

/* first write fualt on an anon VMA without anon_vma, get a fresh
 * anon_vma + AVC that puts the VMA in the tree so rmap can find the pages
 */

/* NOTE: relies on caller holding mm->lock, when faults run concurrently
 * under read lock, needs double checked allocation against a per-mm lock */
enum err vma_range_anon_prepare(struct vma_range *vma_range) {
    if (vma_range->anon_vma)
        return ERR_OK;

    struct anon_vma *av = anon_vma_alloc();
    if (!av)
        return ERR_NO_MEM;

    struct anon_vma_chain *avc = avc_alloc();
    if (!avc) {
        anon_vma_free(av); /* never linked, refcount still 0 */
        return ERR_NO_MEM;
    }

    refcount_inc(&av->refcount); /* the AVC about to link pins this object */
    vma_range->anon_vma = av;
    avc_link(vma_range, av, avc);
    return ERR_OK;
}

enum err vma_range_set_prot(struct vma_range *vma_range,
                            enum vma_range_protection prot) {
    vma_range->prot = prot;
    /* TODO: walk [start,end) and rewrite the PTE permission bits to
     * match, then TLB-shootdown the range. Until page tables are wired, already
     * mapped pages keep their old permissions and only new faults see `prot`.
     */
    return ERR_OK;
}

struct vma_range *vma_range_find(struct mm *mm, vaddr_t addr) {
    struct vma_range *v = mm_vma_range_find(mm, addr);
    return (v && vma_range_start(v) <= addr) ? v : NULL;
}

struct vma_range *vma_range_find_intersection(struct mm *mm, vaddr_t s,
                                              vaddr_t e) {
    return mm_vma_range_find_intersection(mm, s, e);
}

struct vma_range *vma_range_next(struct vma_range *vma_range) {
    struct rbit_node *n = rbit_next(&vma_range->mm_node);
    return n ? rbit_entry(n, struct vma_range, mm_node) : NULL;
}

struct vma_range *vma_range_prev(struct vma_range *vma_range) {
    struct rbit_node *n = rbit_prev(&vma_range->mm_node);
    return n ? rbit_entry(n, struct vma_range, mm_node) : NULL;
}

/* split `vma_range` at `addr`, keeping the low half in `vma_range`
 *
 * returned VMA is the high half, both staying in mm->vma_range_tree
 *
 * caller holds mm->lock */
struct vma_range *vma_range_split(struct vma_range *vma_range, vaddr_t addr) {
    if (!IS_PAGE_ALIGNED(addr))
        return NULL;

    if (addr <= vma_range_start(vma_range) || addr >= vma_range_end(vma_range))
        return NULL;

    struct mm *mm = vma_range->mm;
    vaddr_t old_start = vma_range_start(vma_range);
    vaddr_t old_end = vma_range_end(vma_range);

    struct vma_range *new = vma_range_alloc(mm, addr, old_end, vma_range->prot);
    if (!new)
        return NULL;

    new->pgoff = vma_range->pgoff + ((addr - old_start) >> PAGE_4K_SHIFT);

    /* mirror anon linkage onto the new half */
    if (vma_range->anon_vma) {
        new->anon_vma = vma_range->anon_vma;
        if (anon_vma_clone(new, vma_range) != ERR_OK) {
            new->anon_vma = NULL;
            vma_range_free(new);
            return NULL;
        }
    }

    /* shrink original to low half, re-keying in every tree it lives in
     *
     * interval.low is unchaged */
    mm_vma_range_remove(mm, vma_range);
    vma_range->mm_node.interval.high = addr - 1;

    struct anon_vma_chain *avc;
    list_for_each_entry(avc, &vma_range->anon_vma_chain, same_vma_range)
        avc_rekey(avc);

    mm_vma_range_insert(mm, vma_range);
    mm_vma_range_insert(mm, new);
    return new;
}
