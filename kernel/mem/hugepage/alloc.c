#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>

#include "internal.h"

static size_t find_free_range(struct hugepage *hp, size_t page_count) {
    size_t max = HUGEPAGE_SIZE_IN_4KB_PAGES - page_count;

    for (size_t i = 0; i <= max; i++) {
        bool found = true;
        for (size_t j = 0; j < page_count; j++) {
            if (test_bit(hp, i + j)) {
                found = false;
                i += j;
                break;
            }
        }
        if (found)
            return (size_t) i;
    }

    /* Can't find it */
    return (size_t) -1;
}

static void *do_fastpath_alloc(struct hugepage *hp, bool iflag) {
    size_t start = hp->last_allocated_idx;
    for (size_t i = 0; i < HUGEPAGE_SIZE_IN_4KB_PAGES; i++) {
        size_t idx = (start + i) % HUGEPAGE_SIZE_IN_4KB_PAGES;
        if (!test_bit(hp, idx)) {
            set_bit(hp, idx);
            hp->pages_used++;
            hp->last_allocated_idx = (idx + 1) % HUGEPAGE_SIZE_IN_4KB_PAGES;
            hugepage_unlock(hp, iflag);
            return hugepage_idx_to_addr(hp, idx);
        }
    }

    hugepage_unlock(hp, iflag);
    return NULL;
}

static inline bool alloc_requires_multiple_hugepages(size_t page_count) {
    return hugepage_hps_needed_for(page_count) > 1;
}

void *hugepage_alloc_from_hugepage(struct hugepage *hp, size_t page_count) {
    /* Impossible */
    if (unlikely(alloc_requires_multiple_hugepages(page_count)))
        return NULL;

    bool iflag = hugepage_lock(hp);

    if (page_count == 1)
        return do_fastpath_alloc(hp, iflag);

    size_t idx = find_free_range(hp, page_count);
    if (idx == (size_t) -1) {
        hugepage_unlock(hp, iflag);
        return NULL;
    }

    for (size_t i = 0; i < page_count; i++)
        set_bit(hp, idx + i);

    hp->pages_used += page_count;
    hugepage_unlock(hp, iflag);

    return hugepage_idx_to_addr(hp, idx);
}

static inline bool hugepage_is_not_full(struct hugepage *hp) {
    return !hugepage_is_full(hp);
}

static inline void reinsert_hugepage(struct hugepage *hp, bool locked) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);

    /* Re-insert if it is not full to balance the minheap */
    if (likely(hugepage_is_not_full(hp)))
        hugepage_core_list_insert(hcl, hp, locked);
}

static inline void *alloc_and_adjust(struct hugepage *hp, size_t pages,
                                     bool minheap_locked) {
    kassert(pages > 0);
    if (hugepage_num_pages_free(hp) < pages)
        return NULL;

    hugepage_sanity_assert(hp);
    hugepage_remove_from_core_list_safe(hp, minheap_locked);

    void *ret = hugepage_alloc_from_hugepage(hp, pages);

    reinsert_hugepage(hp, minheap_locked);
    return ret;
}

static void *alloc_search_core_list(struct hugepage_core_list *hcl,
                                    size_t page_count) {
    if (unlikely(alloc_requires_multiple_hugepages(page_count)))
        return NULL;

    struct minheap_node *mhn;
    bool iflag = hugepage_list_lock(hcl);

    minheap_for_each(hcl->hugepage_minheap, mhn) {
        struct hugepage *hp = hugepage_from_minheap_node(mhn);
        void *p = alloc_and_adjust(hp, page_count, true);
        if (p) {
            hugepage_list_unlock(hcl, iflag);
            return p;
        }
    }

    hugepage_list_unlock(hcl, iflag);
    /* No space in minheap */
    return NULL;
}

static struct hugepage **create_hp_arr(size_t needed) {
    core_t owner = get_this_core_id();
    struct hugepage **hp_arr = kzalloc(sizeof(struct hugepage *) * needed);
    if (!hp_arr)
        return NULL;

    if (!hugepage_create_contiguous(owner, needed, hp_arr)) {
        kfree(hp_arr);
        return NULL;
    }

    return hp_arr;
}

static inline void *
alloc_from_hugepage_at_base_and_adjust(struct hugepage *hp, size_t pages,
                                       bool minheap_locked) {
    kassert(pages > 0);

    if (hugepage_num_pages_free(hp) < pages)
        return NULL;

    hugepage_sanity_assert(hp);
    hugepage_remove_from_core_list_safe(hp, minheap_locked);

    bool iflag = hugepage_lock(hp);

    /* sanity: ensure first 'pages' bits are free */
    for (size_t i = 0; i < pages; i++) {
        if (test_bit(hp, i)) {
            hugepage_unlock(hp, iflag);
            reinsert_hugepage(hp, minheap_locked);
            return NULL;
        }
    }

    for (size_t i = 0; i < pages; i++)
        set_bit(hp, i);

    hp->pages_used += pages;
    hugepage_unlock(hp, iflag);

    reinsert_hugepage(hp, minheap_locked);
    return hugepage_idx_to_addr(hp, 0);
}

static void *alloc_from_multiple_hugepages(size_t page_count) {
    size_t needed = hugepage_hps_needed_for(page_count);
    struct hugepage **hp_arr = create_hp_arr(needed);
    if (!hp_arr)
        return NULL;

    void *first_ptr = NULL;

    for (size_t i = 0; i < needed; i++) {
        size_t chunk = hugepage_chunk_for(page_count);
        struct hugepage *hp = hp_arr[i];

        void *p = alloc_from_hugepage_at_base_and_adjust(hp, chunk, false);
        if (!p) {
            kfree(hp_arr);
            return NULL;
        }

        if (i == 0)
            first_ptr = p;

        page_count -= chunk;
    }

    kfree(hp_arr);
    return first_ptr;
}

static inline void *try_alloc_from_gc_list(struct hugepage_core_list *hcl,
                                           size_t page_count) {
    struct hugepage *recycled = hugepage_get_from_gc_list();
    if (!recycled)
        return NULL;

    recycled->owner_core = hcl->core_num;
    hugepage_insert_internal(recycled);
    return alloc_and_adjust(recycled, page_count, false);
}

static void *alloc_from_new_hugepage(struct hugepage_core_list *hcl,
                                     size_t page_count) {
    struct hugepage *new = hugepage_create_internal(hcl->core_num);
    return alloc_and_adjust(new, page_count, false);
}

/* TODO: Fallback to stealing hugepages from other cores */
void *hugepage_alloc_pages(size_t page_count) {
    struct hugepage_core_list *hcl = hugepage_this_core_list();

    /* Fastpath */
    void *ptr = alloc_search_core_list(hcl, page_count);
    if (ptr)
        return ptr;

    if (unlikely(alloc_requires_multiple_hugepages(page_count)))
        return alloc_from_multiple_hugepages(page_count);

    ptr = try_alloc_from_gc_list(hcl, page_count);
    if (ptr)
        return ptr;

    return alloc_from_new_hugepage(hcl, page_count);
}
