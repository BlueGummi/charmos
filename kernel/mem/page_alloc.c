#include <math/units.h>
#include <mem/address_range.h>
#include <mem/asan.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/page_alloc.h>
#include <mem/page_fault.h>
#include <mem/pmm.h>
#include <mem/vas.h>

ALLOC_FLAG_EX_REGISTER(PG, CONTIGUOUS);
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
ADDRESS_RANGE_DEFINE(page_alloc, .align = PAGE_SIZE,
                     .flags = ADDRESS_RANGE_DYNAMIC, .size = TIB(8),
                     .page_fault_handler = &page_alloc_pfh);

void page_alloc_init() {
    page_alloc_vas = vas_bootstrap_from(&ADDRESS_RANGE(page_alloc));
}

static void page_alloc_vas_release(vaddr_t virt, size_t page_count,
                                   size_t nr_mapped) {
    for (size_t i = 0; i < nr_mapped; i++) {
        vaddr_t vaddr = virt + i * PAGE_SIZE;
        paddr_t phys = (paddr_t) vmm_get_phys(vaddr);
        vmm_unmap_page(vaddr);

        if (phys != PADDR_MAX)
            pmm_free_page(phys);
    }

    vas_free(page_alloc_vas, virt, page_count * PAGE_SIZE);
}

static void *page_alloc_vas_mapped_pages(size_t page_count,
                                         enum alloc_flags flags,
                                         bool demand_paged) {
    vaddr_t virt = vas_alloc(page_alloc_vas, page_count * PAGE_SIZE, PAGE_SIZE);
    if (!virt)
        return NULL;

    page_flags_t page_flags = PAGE_PRESENT | PAGE_WRITE | PAGE_XD;
    bool zero = flags & ALLOC_FLAG_ZERO_ON_ALLOC;

    for (size_t i = 0; i < page_count; i++) {
        vaddr_t vaddr = virt + i * PAGE_SIZE;

        if (!demand_paged || !zero) {
            uintptr_t phys = pmm_alloc_page(flags);
            if (!phys) {
                page_alloc_vas_release(virt, page_count, i);
                return NULL;
            }

            if (vmm_map_page(vaddr, phys, .page_flags = page_flags) < 0) {
                pmm_free_page(phys);
                page_alloc_vas_release(virt, page_count, i);
                return NULL;
            }
        } else {
            enum err e =
                vmm_mark_demand_page(vaddr, DEMAND_PAGE_FLAG_ZERO_MEMORY |
                                                DEMAND_PAGE_FLAG_WRITABLE);
            if (e < 0) {
                page_alloc_vas_release(virt, page_count, i);
                return NULL;
            }
        }
    }

    return (void *) virt;
}

/* Must be in vas, that's the only check */
static bool page_alloc_pf_valid(struct page_fault_info *pfi) {
    return vas_vaddr_is_allocated(page_alloc_vas, pfi->addr);
}

void *page_alloc_full(size_t page_count, struct alloc_params params) {
    void *ret;
    if (page_count == 1 || params.flags & ALLOC_FLAG_EX(PG, CONTIGUOUS)) {
        paddr_t phys = pmm_alloc_pages(page_count);
        if (!phys)
            return NULL;

        ret = hhdm_paddr_to_ptr(phys);
    } else {
        ret = page_alloc_vas_mapped_pages(page_count, params.flags, false);
    }

#ifdef DEBUG_ASAN
    if (ret)
        asan_unpoison(ret, page_count * PAGE_SIZE);
#endif

    return ret;
}

void *page_alloc_demand_full(size_t page_count, struct alloc_params params) {
    void *ret = page_alloc_vas_mapped_pages(page_count, params.flags, true);

#ifdef DEBUG_ASAN
    /* NOTE: these pages are not yet backed; the shadow write here assumes the
     * shadow itself is mapped for this VA range. */
    if (ret)
        asan_unpoison(ret, page_count * PAGE_SIZE);
#endif

    return ret;
}

void page_free_full(void *ptr, size_t page_count, enum alloc_behavior b) {
    cc_ignore(b);

#ifdef DEBUG_ASAN
    if (ptr)
        asan_poison(ptr, page_count * PAGE_SIZE);
#endif

    if (hhdm_ptr_in_range(ptr)) {
        pmm_free_pages(hhdm_ptr_to_paddr(ptr), page_count);
    } else {
        page_alloc_vas_release((vaddr_t) ptr, page_count, page_count);
    }
}

bool page_alloc_vaddr_in_vas(vaddr_t vaddr) {
    return vas_vaddr_in_vas(page_alloc_vas, vaddr) ||
           hhdm_vaddr_in_range(vaddr);
}
