#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>

/* Just to prevent OOB */
#define assert_u64_idx_idx_sanity(u64_idx)                                     \
    kassert(u64_idx < HUGEPAGE_U64_BITMAP_SIZE)

static inline void *hugepage_idx_to_addr(struct hugepage *hp, size_t idx) {
    return (void *) (hp->virt_base + idx * PAGE_SIZE);
}

static inline size_t u64_idx_for_idx(size_t idx) {
    size_t u64_idx = idx / 64;
    assert_u64_idx_idx_sanity(u64_idx);
    return u64_idx;
}

static inline void set_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t mask = 1 << (index % 64);
    hp->bitmap[u64_idx] |= mask;
}

static inline void clear_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t mask = ~(1 << (index % 64));
    hp->bitmap[u64_idx] &= mask;
}

static inline bool test_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t value = hp->bitmap[u64_idx];
    return (value & (1 << (index % 64))) != 0;
}

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

/* This is not supposed to remove from the per-core
 * minheap if the hugepage is full */
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

/* This will not garbage collect the
 * hugepage if it becomes fully empty */
void hugepage_free_from_hugepage(struct hugepage *hp, void *ptr,
                                 size_t page_count) {
    kassert(page_count > 0);
    uintptr_t addr = (uintptr_t) ptr;
    kassert(addr >= hp->virt_base);
    size_t offset = addr - hp->virt_base;
    kassert(offset % PAGE_SIZE == 0);

    size_t index = offset / PAGE_SIZE;
    kassert(index + page_count <= HUGEPAGE_SIZE_IN_4KB_PAGES);

    bool iflag = hugepage_lock(hp);

    /* Sanity check: all bits must be set */
    for (size_t i = 0; i < page_count; i++)
        if (!test_bit(hp, index + i))
            k_panic("double free or corrupt ptr");

    for (size_t i = 0; i < page_count; i++)
        clear_bit(hp, index + i);

    kassert(hp->pages_used >= page_count);
    hp->pages_used -= page_count;

    hugepage_unlock(hp, iflag);
}

static inline bool hugepage_is_not_full(struct hugepage *hp) {
    return !hugepage_is_full(hp);
}

static inline void reinsert_hugepage(struct hugepage *hp) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);

    /* Re-insert if it is not full to balance the minheap */
    if (likely(hugepage_is_not_full(hp)))
        hugepage_core_list_insert(hcl, hp);
}

static inline void *alloc_and_adjust(struct hugepage *hp, size_t pages) {
    kassert(pages > 0);
    if (hugepage_num_pages_free(hp) < pages)
        return NULL;

    hugepage_sanity_assert(hp);
    hugepage_remove_from_list_safe(hp);
    void *ret = hugepage_alloc_from_hugepage(hp, pages);
    reinsert_hugepage(hp);
    return ret;
}

static void *alloc_search_core_list(struct hugepage_core_list *hcl,
                                    size_t page_count) {
    if (unlikely(alloc_requires_multiple_hugepages(page_count)))
        return NULL;

    struct minheap_node *mhn;
    minheap_for_each(hcl->hugepage_minheap, mhn) {
        struct hugepage *hp = hugepage_from_minheap_node(mhn);
        void *p = alloc_and_adjust(hp, page_count);
        if (p)
            return p;
    }

    /* No space in minheap */
    return NULL;
}

static struct hugepage **create_hp_arr(size_t needed) {
    core_t owner = get_this_core_id();
    struct hugepage **hp_arr = kmalloc(sizeof(struct hugepage *) * needed);
    if (!hugepage_create_contiguous(owner, needed, hp_arr)) {
        kfree(hp_arr);
        return NULL;
    }
    return hp_arr;
}

static void *alloc_from_multiple_hugepages(size_t page_count) {
    size_t needed = hugepage_hps_needed_for(page_count);
    struct hugepage **hp_arr = create_hp_arr(needed);
    if (!hp_arr)
        return NULL;

    for (size_t i = 0; i < needed; i++) {
        size_t chunk = page_count > HUGEPAGE_SIZE_IN_4KB_PAGES
                           ? HUGEPAGE_SIZE_IN_4KB_PAGES
                           : page_count;

        struct hugepage *hp = hp_arr[i];
        alloc_and_adjust(hp, chunk);

        page_count -= chunk;
    }

    vaddr_t base = hp_arr[0]->virt_base;
    kfree(hp_arr);
    return (void *) base;
}

static inline void *try_alloc_from_gc_list(struct hugepage_core_list *hcl,
                                           size_t page_count) {
    struct hugepage *recycled = hugepage_get_from_gc_list();
    if (!recycled)
        return NULL;

    recycled->owner_core = hcl->core_num;
    hugepage_insert_internal(recycled);
    return alloc_and_adjust(recycled, page_count);
}

static void *alloc_from_new_hugepage(struct hugepage_core_list *hcl,
                                     size_t page_count) {
    struct hugepage *new = hugepage_create_internal(hcl->core_num);
    return alloc_and_adjust(new, page_count);
}

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
