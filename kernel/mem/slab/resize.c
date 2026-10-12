#include <math/bit.h>
#include <math/range.h>
#include <mem/domain.h>
#include <mem/pmm.h>

#include "internal.h"

bool slab_can_resize_to(struct slab *slab, size_t new_size_pages) {
    size_t cap = next_pow2(slab->page_count);
    size_t min = (cap >> 1) + 1;
    return IN_RANGE(new_size_pages, min, cap);
}

static void slab_shrink(struct slab *slab, size_t start, size_t end,
                        bool assert_nonnull) {
    for (size_t i = start; i < end; i++) {
        struct page *page = atomic_load_rlx(&slab->backing_pages[i]);
        if (assert_nonnull)
            enforce(page);

        if (!page)
            return;

        paddr_t phys = page_get_paddr(page);
        vaddr_t virt = (vaddr_t) slab + i * PAGE_SIZE;
        vmm_unmap_page(virt);
        pmm_free_page(phys);
        atomic_store_rlx(&slab->backing_pages[i], NULL);
    }

    slab->page_count = start;
}

/* returns if the resize was successful or not needed */
bool slab_resize(struct slab *slab, size_t new_size_pages) {
    enforce(slab_can_resize_to(slab, new_size_pages));
    size_t old = slab->page_count;

    if (old == new_size_pages)
        return true;

    struct slab_domain *parent = slab->parent_cache->parent_domain;

    /* Grow */
    if (old < new_size_pages) {
        for (size_t i = old; i < new_size_pages; i++)
            atomic_store_rlx(&slab->backing_pages[i], NULL);

        for (size_t i = old; i < new_size_pages; i++) {
            vaddr_t virt = (vaddr_t) slab + i * PAGE_SIZE;

            /* TODO: we can handle in the PAGEABLE type that isn't zero
             * with stuff once we get the infra for that to exist */
            if (slab->type == SLAB_TYPE_PAGEABLE_ZERO) {
                /* TODO: memory commit limits here */

                /* TODO: grow_err needs to handle pages that were just
                 * marked as being demand paged, i.e. vmm_unmap_page should
                 * either have its behavior changed or another function should
                 * get introduced for the sake of this little thing */
                if (cc_unlikely(vmm_mark_demand_page(
                                    virt, DEMAND_PAGE_FLAG_WRITABLE |
                                              DEMAND_PAGE_FLAG_ZERO_MEMORY) <
                                0)) {
                    goto grow_err;
                }
            } else {
                paddr_t phys = domain_alloc_from_domain(parent->domain, 1);
                if (!phys)
                    goto grow_err;

                uint64_t flags = slab_page_flags(slab->type);
                if (cc_unlikely(vmm_map_page(virt, phys, .page_flags = flags) <
                                0)) {
                    pmm_free_page(phys); /* not yet recorded, free directly */
                    goto grow_err;
                }

                atomic_store_rlx(&slab->backing_pages[i], page_for_paddr(phys));
                if (slab->type == SLAB_TYPE_NONPAGEABLE_ZERO)
                    memset(hhdm_paddr_to_ptr(phys), 0, PAGE_SIZE);
            }
        }

        slab->page_count = new_size_pages;
        return true;

    grow_err:
        slab_shrink(slab, old, new_size_pages, /* assert_nonnull = */ false);
        return false;
    }

    slab_shrink(slab, new_size_pages, old, /* assert_nonnull = */ true);
    return true;
}
