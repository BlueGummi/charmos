#include <math/kb_mb_gb_tb.h>
#include <mem/address_range.h>
#include <mem/asan.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/page_alloc.h>
#include <mem/page_fault.h>
#include <mem/pmm.h>
#include <mem/vas.h>

static bool page_alloc_pf_valid(struct page_fault_info *pfi);

static struct page_fault_handler_ops page_alloc_pfho = {
    .alloc_pages = NULL,
    .update_after_map = NULL,
    .is_valid_fault = page_alloc_pf_valid,
};

static struct page_fault_handler page_alloc_pfh = {
    .ops = &page_alloc_pfho,
};

static struct vas *page_alloc_vas = NULL;
ADDRESS_RANGE_DECLARE(page_alloc, .align = PAGE_SIZE,
                      .flags = ADDRESS_RANGE_DYNAMIC, .size = TB(8),
                      .page_fault_handler = &page_alloc_pfh);

void page_alloc_init() {
    page_alloc_vas = vas_bootstrap_from(&ADDRESS_RANGE(page_alloc));
}

static void *page_alloc_vas_mapped_pages(size_t n_pages, enum alloc_flags flags,
                                         bool demand_paged) {
    vaddr_t virt = vas_alloc(page_alloc_vas, n_pages * PAGE_SIZE, PAGE_SIZE);
    if (!virt)
        return NULL;

    uintptr_t phys_pages[n_pages];
    uint64_t allocated = 0;

    page_flags_t page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_XD;
    bool zero = flags & ALLOC_FLAG_ZERO_ON_ALLOC;

    for (uint64_t i = 0; i < n_pages; i++) {
        if (!demand_paged || !zero) {
            uintptr_t phys = pmm_alloc_page(flags);
            if (!phys) {
                for (uint64_t j = 0; j < allocated; j++)
                    pmm_free_page(phys_pages[j]);
                return NULL;
            }

            enum errno e = vmm_map_page(virt + i * PAGE_SIZE, phys, page_flags);
            if (e < 0) {
                pmm_free_page(phys);
                for (uint64_t j = 0; j < allocated; j++)
                    pmm_free_page(phys_pages[j]);

                return NULL;
            }

            phys_pages[allocated++] = phys;
        } else {
            enum errno e = vmm_mark_demand_page(virt + i * PAGE_SIZE,
                                                DEMAND_PAGE_FLAG_ZERO_MEMORY |
                                                    DEMAND_PAGE_FLAG_WRITABLE);
            if (e < 0) {
                for (size_t i = 0; i < allocated; i++) {
                    vaddr_t v = virt + i * PAGE_SIZE;
                    vmm_unmap_page(v);
                }

                return NULL;
            }
            allocated++;
        }
    }

    return (void *) virt;
}

/* Must be in vas, that's the only check */
static bool page_alloc_pf_valid(struct page_fault_info *pfi) {
    return vas_vaddr_is_allocated(page_alloc_vas, pfi->addr);
}

void *page_alloc_internal(size_t n_pages, enum alloc_flags flags,
                          enum alloc_behavior bh) {
    void *ret;
    if (n_pages == 1 || flags & ALLOC_FLAG_CONTIGUOUS) {
        paddr_t phys = pmm_alloc_pages(n_pages);
        if (!phys)
            return NULL;

        ret = hhdm_paddr_to_ptr(phys);
    } else {
        ret = page_alloc_vas_mapped_pages(n_pages, flags, false);
    }

#ifdef DEBUG_ASAN
    if (ret)
        asan_unpoison(ret, n_pages * PAGE_SIZE);
#endif
    return ret;
}

void *page_alloc_demand_internal(size_t n_pages, enum alloc_flags flags,
                                 enum alloc_behavior bh) {
    void *ret = page_alloc_vas_mapped_pages(n_pages, flags, true);

#ifdef DEBUG_ASAN
    /* NOTE: these pages are not yet backed; the shadow write here assumes the
     * shadow itself is mapped for this VA range. */
    if (ret)
        asan_unpoison(ret, n_pages * PAGE_SIZE);
#endif
    return ret;
}

void page_free_internal(void *ptr, size_t n_pages, enum alloc_behavior b) {
#ifdef DEBUG_ASAN
    if (ptr)
        asan_poison(ptr, n_pages * PAGE_SIZE);
#endif

    if (hhdm_ptr_in_range(ptr)) {
        pmm_free_pages(hhdm_ptr_to_paddr(ptr), n_pages);
    } else {
        vaddr_t virt = (vaddr_t) ptr;
        for (uint32_t i = 0; i < n_pages; i++) {
            vaddr_t vaddr = virt + i * PAGE_SIZE;
            paddr_t phys = (paddr_t) vmm_get_phys(vaddr, VMM_FLAG_NONE);
            vmm_unmap_page(vaddr);

            if (phys != PADDR_MAX)
                pmm_free_page(phys);
        }

        vas_free(page_alloc_vas, virt, n_pages * PAGE_SIZE);
    }
}

bool page_alloc_vaddr_in_vas(vaddr_t vaddr) {
    return vas_vaddr_in_vas(page_alloc_vas, vaddr) ||
           hhdm_vaddr_in_range(vaddr);
}
