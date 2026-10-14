#include <math/bit.h>
#include <mem/anon_vma.h>
#include <mem/fixed_size_alloc.h>
#include <mem/folio.h>
#include <mem/pmm.h>
#include <smp/perdomain.h>
#include <string.h>
#include <types/refcount.h>

FIXED_SIZE_RANGE_PERDOMAIN_DEFINE(folio, .obj_size = sizeof(struct folio),
                                  .obj_align = PAGE_PAYLOAD_ALIGNMENT);

struct folio *folio_alloc_folio_struct() {
    return FSR_PERDOMAIN_ALLOC(folio);
}

void folio_free_folio_struct(struct folio *f) {
    folio_unbind_pages(f);
    return FSR_PERDOMAIN_FREE(folio, f);
}

/* TODO: implement later
void folio_free(struct folio *folio) {
    if (folio_is_anon(folio)) {
        anon_vma_putref(folio_get_anon_vma(folio));
    } else if (folio_is_file_vma_range(folio)) {
        // TODO: When we get file VMA
    }

    size_t pages = pow2(folio->order);

    pmm_free_pages(page_get_paddr(folio->base_page), pages);
    folio_free_folio_struct(folio);
} */

struct folio *folio_alloc_full(uint8_t order, struct alloc_params params) {
    size_t pages = pow2(order);
    struct folio *folio = folio_alloc_folio_struct();
    if (!folio)
        return NULL;

    paddr_t phys = pmm_alloc_pages(pages, params.flags);
    if (!phys) {
        folio_free_folio_struct(folio);
        return NULL;
    }

    struct page *base_page = page_for_paddr(phys);
    folio->base_page = base_page;
    folio->order = order;
    atomic_init(&folio->mapcount, 0);
    refcount_init(&folio->refcount, 1);
    folio->mapping = NULL;
    atomic_init(&folio->flags, 0);
    folio_bind_pages(folio);

    return folio;
}

void folio_zero(struct folio *f) {
    vaddr_t vaddr;
    folio_for_each_page_vaddr(vaddr, f) {
        memset((void *) vaddr, 0, PAGE_SIZE);
    }
}

void folio_copy(const struct folio *src, struct folio *dst) {
    enforce(src->order == dst->order);
    size_t nr_pages = folio_nr_pages(src);
    for (size_t i = 0; i < nr_pages; i++) {
        vaddr_t src_vaddr = folio_get_vaddr_for(src, i);
        vaddr_t dst_vaddr = folio_get_vaddr_for(dst, i);
        memcpy((void *) dst_vaddr, (void *) src_vaddr, PAGE_SIZE);
    }
}

void folio_bind_pages(struct folio *f) {
    struct page *page;
    folio_for_each_page_struct(page, f) {
        page_set_folio(page, f);
    }
}

void folio_unbind_pages(struct folio *f) {
    struct page *page;
    folio_for_each_page_struct(page, f) {
        page_clear_folio(page);
    }
}

bool page_is_folio_head(struct page *p) {
    return page_get_folio(p)->base_page == p;
}

uint32_t page_get_folio_index(struct page *p) {
    struct folio *f = page_get_folio(p);
    return page_get_pfn(p) - page_get_pfn(f->base_page);
}
