#include <asm.h>
#include <console/printf.h>
#include <drivers/iommu/vt_d.h>
#include <drivers/mmio.h>
#include <log.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

#include "internal.h"

static bool vtd_pt_empty(uint64_t *pt) {
    for (size_t i = 0; i < SL_ENTRY_COUNT; i++) {
        if (pt[i] & SL_PTE_READ)
            return false;
    }
    return true;
}

static uint64_t *vtd_sl_ensure_child(sl_pte_atomic_t *entry) {
    uint64_t val = atomic_load_explicit(entry, memory_order_relaxed);
    val &= ~SL_PTE_LOCK_BIT;

    if (val & SL_PTE_READ)
        return hhdm_paddr_to_ptr(SL_PTE_ADDR(val));

    paddr_t phys = pmm_alloc_page();
    if (!phys)
        return NULL;

    void *virt = hhdm_paddr_to_ptr(phys);
    memset(virt, 0, PAGE_SIZE);

    atomic_store_explicit(entry, SL_TABLE_ENTRY(phys) | SL_PTE_LOCK_BIT,
                          memory_order_release);
    return virt;
}

static enum iommu_error vtd_sl_map_page(uint64_t *sl_pgd, iova_t iova,
                                        paddr_t pa, uint32_t perm) {
    sl_pte_atomic_t *locked[PT_LEVELS] = {NULL, NULL, NULL, NULL};
    enum irql saved[PT_LEVELS];

    sl_pte_atomic_t *e4 = (sl_pte_atomic_t *) &sl_pgd[SL_PML4_INDEX(iova)];
    saved[0] = vtd_pt_lock(e4);
    locked[0] = e4;

    uint64_t *pdp = vtd_sl_ensure_child(e4);
    if (!pdp)
        goto fail;

    sl_pte_atomic_t *e3 = (sl_pte_atomic_t *) &pdp[SL_PDPT_INDEX(iova)];
    saved[1] = vtd_pt_lock(e3);
    locked[1] = e3;

    uint64_t *pd = vtd_sl_ensure_child(e3);
    if (!pd)
        goto fail;

    sl_pte_atomic_t *e2 = (sl_pte_atomic_t *) &pd[SL_PD_INDEX(iova)];
    saved[2] = vtd_pt_lock(e2);
    locked[2] = e2;

    uint64_t *pt = vtd_sl_ensure_child(e2);
    if (!pt)
        goto fail;

    sl_pte_atomic_t *e1 = (sl_pte_atomic_t *) &pt[SL_PT_INDEX(iova)];
    saved[3] = vtd_pt_lock(e1);
    locked[3] = e1;

    atomic_store_explicit(e1, SL_PAGE_ENTRY(pa, perm), memory_order_release);

    /* Unlock in reverse: leaf → root */
    for (int i = PT_LEVELS - 1; i >= 0; i--)
        vtd_pt_unlock(locked[i], saved[i]);

    return IOMMU_ERR_OK;

fail:
    for (int i = PT_LEVELS - 1; i >= 0; i--) {
        if (locked[i])
            vtd_pt_unlock(locked[i], saved[i]);
    }
    return IOMMU_ERR_NO_MEM;
}

static bool vtd_sl_unmap_page(uint64_t *sl_pgd, iova_t iova) {
    pte_t pte;

    pte = sl_pgd[SL_PML4_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return false;

    uint64_t *pdp = hhdm_paddr_to_ptr(SL_PTE_ADDR(pte));
    pte = pdp[SL_PDPT_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return false;

    uint64_t *pd = hhdm_paddr_to_ptr(SL_PTE_ADDR(pte));
    pte = pd[SL_PD_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return false;

    uint64_t *pt = hhdm_paddr_to_ptr(SL_PTE_ADDR(pte));
    if (!(pt[SL_PT_INDEX(iova)] & SL_PTE_READ))
        return false;

    pt[SL_PT_INDEX(iova)] = 0;
    return true;
}

static paddr_t vtd_sl_translate(uint64_t *sl_pgd, iova_t iova) {
    pte_t pte;

    pte = sl_pgd[SL_PML4_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return 0;

    uint64_t *pdp = hhdm_paddr_to_ptr(SL_PTE_ADDR(pte));
    pte = pdp[SL_PDPT_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return 0;

    uint64_t *pd = hhdm_paddr_to_ptr(SL_PTE_ADDR(pte));
    pte = pd[SL_PD_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return 0;

    uint64_t *pt = hhdm_paddr_to_ptr(SL_PTE_ADDR(pte));
    pte = pt[SL_PT_INDEX(iova)];
    if (!(pte & SL_PTE_READ))
        return 0;

    return SL_PTE_ADDR(pte) | SL_PAGE_OFFSET(iova);
}

static enum iommu_error vtd_map(struct iommu_domain *domain, iova_t iova,
                                paddr_t pa, size_t size, uint32_t perm) {
    struct vtd_unit *u = domain->unit->private;
    struct vtd_domain *vd = domain->priv;

    if (!IS_PAGE_ALIGNED(iova) || !IS_PAGE_ALIGNED(pa) ||
        !IS_PAGE_ALIGNED(size))
        return IOMMU_ERR_INVALID;

    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        enum iommu_error err =
            vtd_sl_map_page(vd->sl_pgd, iova + offset, pa + offset, perm);
        if (err != IOMMU_ERR_OK)
            return err;
    }

    vtd_iq_submit(u, IOTLB_INVAL_DESC_DOMAIN(vd->domain_id));
    vtd_iq_flush(u);

    return IOMMU_ERR_OK;
}

static void vtd_flush_iotlb_domain(struct iommu_domain *domain) {
    struct vtd_unit *u = domain->unit->private;
    struct vtd_domain *vd = domain->priv;

    vtd_iq_submit(u, IOTLB_INVAL_DESC_DOMAIN(vd->domain_id));
    vtd_iq_flush(u);
}

static void vtd_flush_iotlb_range(struct iommu_domain *domain, iova_t iova,
                                  size_t size) {
    struct vtd_unit *u = domain->unit->private;
    struct vtd_domain *vd = domain->priv;

    if (CAP_PAGE_SELECTIVE_INVALIDATION(u->cap) && size <= 32 * PAGE_SIZE) {
        for (size_t off = 0; off < size; off += PAGE_SIZE)
            vtd_iq_submit(u,
                          IOTLB_INVAL_DESC_PAGE(vd->domain_id, iova + off, 0));
    } else {
        vtd_iq_submit(u, IOTLB_INVAL_DESC_DOMAIN(vd->domain_id));
    }

    vtd_iq_flush(u);
}

struct vtd_walk_state {
    uint64_t *tables[PT_LEVELS];
    size_t indices[PT_LEVELS];
};

static bool vtd_sl_unmap_locked(uint64_t *sl_pgd, iova_t iova,
                                struct vtd_walk_state *ws) {
    sl_pte_atomic_t *locked[PT_LEVELS];
    enum irql saved[PT_LEVELS];

    ws->tables[0] = sl_pgd;
    ws->indices[0] = SL_PML4_INDEX(iova);
    ws->indices[1] = SL_PDPT_INDEX(iova);
    ws->indices[2] = SL_PD_INDEX(iova);
    ws->indices[3] = SL_PT_INDEX(iova);

    uint64_t *cur = sl_pgd;

    for (int lvl = 0; lvl < PT_LEVELS; lvl++) {
        sl_pte_atomic_t *entry = (sl_pte_atomic_t *) &cur[ws->indices[lvl]];
        saved[lvl] = vtd_pt_lock(entry);
        locked[lvl] = entry;

        uint64_t val = atomic_load_explicit(entry, memory_order_relaxed);
        val &= ~SL_PTE_LOCK_BIT;

        if (!(val & SL_PTE_READ)) {
            /* not mapped, unlock everything and bail */
            for (int i = lvl; i >= 0; i--)
                vtd_pt_unlock(locked[i], saved[i]);
            return false;
        }

        if (lvl == PT_LEVELS - 1) {
            /* leaf, clear it, keep lock bit until unlock */
            atomic_store_explicit(entry, SL_PTE_LOCK_BIT, memory_order_release);
        } else {
            ws->tables[lvl + 1] = hhdm_paddr_to_ptr(SL_PTE_ADDR(val));
            cur = ws->tables[lvl + 1];
        }
    }

    for (int i = PT_LEVELS - 1; i >= 0; i--)
        vtd_pt_unlock(locked[i], saved[i]);

    return true;
}

/* bottom-up reclaim of empty intermediate tables */
static void vtd_sl_reclaim_walk(struct vtd_walk_state *ws) {
    /* lvl 3 is the leaf (PT entry) which is already cleared */
    for (int lvl = PT_LEVELS - 2; lvl >= 0; lvl--) {
        sl_pte_atomic_t *parent =
            (sl_pte_atomic_t *) &ws->tables[lvl][ws->indices[lvl]];

        enum irql old_irql = vtd_pt_lock(parent);

        uint64_t val = atomic_load_explicit(parent, memory_order_relaxed);
        val &= ~SL_PTE_LOCK_BIT;

        if (!(val & SL_PTE_READ)) {
            /* someone else already freed it */
            vtd_pt_unlock(parent, old_irql);
            break;
        }

        uint64_t *child = ws->tables[lvl + 1];

        if (!vtd_pt_empty(child)) {
            vtd_pt_unlock(parent, old_irql);
            break;
        }

        /* child is empty, clear parent entry and free child */
        atomic_store_explicit(parent, SL_PTE_LOCK_BIT, memory_order_release);
        vtd_pt_mark_dead(parent);
        vtd_pt_unlock(parent, old_irql);

        pmm_free_page(hhdm_ptr_to_paddr(child));
    }
}

static void vtd_unmap(struct iommu_domain *domain, iova_t iova, size_t size) {
    struct vtd_unit *u = domain->unit->private;
    struct vtd_domain *vd = domain->priv;

    if (!IS_PAGE_ALIGNED(iova) || !IS_PAGE_ALIGNED(size))
        return;

    size_t page_count = size / PAGE_SIZE;

    struct vtd_walk_state *walks = kmalloc(page_count * sizeof(*walks));
    bool *unmapped = kmalloc(page_count * sizeof(bool));
    if (!walks || !unmapped) {
        for (size_t off = 0; off < size; off += PAGE_SIZE) {
            struct vtd_walk_state dummy;
            vtd_sl_unmap_locked(vd->sl_pgd, iova + off, &dummy);
        }
        vtd_iotlb_flush_range_batched(u, vd->domain_id, iova, size);
        kfree(walks);
        kfree(unmapped);
        return;
    }

    /* clear all leaves */
    for (size_t i = 0; i < page_count; i++)
        unmapped[i] =
            vtd_sl_unmap_locked(vd->sl_pgd, iova + i * PAGE_SIZE, &walks[i]);

    /* send invalidation */
    vtd_iotlb_flush_range_batched(u, vd->domain_id, iova, size);

    /* go and reclaim */
    for (size_t i = 0; i < page_count; i++) {
        if (unmapped[i])
            vtd_sl_reclaim_walk(&walks[i]);
    }

    kfree(walks);
    kfree(unmapped);
}
