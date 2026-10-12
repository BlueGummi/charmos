#include <mem/demand_page.h>
#include <mem/domain.h>
#include <mem/page_fault.h>

#include "internal.h"

static bool slab_pf_valid(struct page_fault_info *pfi);
static bool slab_pf_update(vaddr_t vaddr, struct page *p);
static paddr_t slab_pf_alloc_pages(vaddr_t vaddr, uint8_t order);

struct page_fault_handler_ops ops = {
    .is_valid_fault = slab_pf_valid,
    .update_after_map = slab_pf_update,
    .alloc_pages = slab_pf_alloc_pages,
};

struct page_fault_handler slab_page_fault_handler = {
    .ops = &ops,
};

/* No need to use up extra cycles to consult the vas, we
 * can check order map, align, and then check if it's mapped */
static bool slab_pf_valid(struct page_fault_info *pfi) {
    return vmm_get_phys((vaddr_t) slab_for_ptr((void *) pfi->addr)) !=
           PADDR_MAX;
}

static bool slab_pf_update(vaddr_t vaddr, struct page *p) {
    struct slab *slab = slab_for_ptr((void *) vaddr);
    vaddr_t slab_vaddr = (vaddr_t) slab;
    pfn_t slab_vpn = PAGE_TO_PFN(slab_vaddr);
    pfn_t vaddr_vpn = PAGE_TO_PFN(vaddr);
    size_t idx = vaddr_vpn - slab_vpn;
    struct page *exp = NULL;

    return atomic_cas_strong(&slab->backing_pages[idx], &exp, p);
}

static paddr_t slab_pf_alloc_pages(vaddr_t vaddr, uint8_t order) {
    enforce(order == 0);
    struct slab *s = slab_for_ptr((void *) vaddr);
    return domain_alloc_from_domain(s->parent_cache->parent_domain->domain, 1);
}
