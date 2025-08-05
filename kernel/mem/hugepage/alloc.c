#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>

/* Just to prevent OOB */
#define assert_byte_idx_sanity(byte) kassert(byte < HUGEPAGE_U8_BITMAP_SIZE)

static inline size_t byte_for_idx(size_t idx) {
    size_t byte = idx / 8;
    assert_byte_idx_sanity(byte);
    return byte;
}

static inline void set_bit(struct hugepage *hp, size_t index) {
    size_t byte = byte_for_idx(index);
    uint8_t mask = 1 << (index % 8);
    hp->bitmap[byte] |= mask;
}

static inline void clear_bit(struct hugepage *hp, size_t index) {
    size_t byte = byte_for_idx(index);
    uint8_t mask = ~(1 << (index % 8));
    hp->bitmap[byte] &= mask;
}

static inline bool test_bit(struct hugepage *hp, size_t index) {
    size_t byte = byte_for_idx(index);
    uint8_t value = hp->bitmap[byte];
    return (value & (1 << (index % 8))) != 0;
}

static size_t find_free_range(struct hugepage *hp, size_t page_count) {
    size_t max = HUGEPAGE_SIZE_IN_4KB_PAGES;

    for (size_t i = 0; i <= max - page_count; i++) {
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
            return (void *) (hp->virt_base + idx * 0x1000);
        }
    }

    hugepage_unlock(hp, iflag);
    return NULL;
}

void *hugepage_alloc_from_hugepage(struct hugepage *hp, size_t page_count) {
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

    return (void *) (hp->virt_base + idx * PAGE_SIZE);
}

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
    for (size_t i = 0; i < page_count; i++) {
        if (!test_bit(hp, index + i)) {
            k_panic("double free or corrupt ptr");
        }
    }

    for (size_t i = 0; i < page_count; i++)
        clear_bit(hp, index + i);

    kassert(hp->pages_used >= page_count);
    hp->pages_used -= page_count;

    hugepage_unlock(hp, iflag);
}
