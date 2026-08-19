/* @title: Folio */
#pragma once
#include <compiler.h>
#include <math/pow.h>
#include <mem/alloc.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <structures/list.h>
#include <types/refcount.h>
#include <types/types.h>

struct anon_vma;
struct file_vma_range;
struct vma_range;

/* folio_flags: 32-bit bitflags
 *
 *      ┌────────────────────────────────────┐
 * Bits │  31..8                 7..4   3..0 │
 * Use  │  (reserved)            ..LU   DMAO │
 *      └────────────────────────────────────┘
 *
 * O - On an LRU / reclaim list
 * A - Anon: mapping is an anon_vma (clear => file_vma_range)
 * M - Mapped at least once (mapcount has been nonzero)
 * D - Dirty: needs writeback
 * U - Uptodate: contents valid
 * L - Locked: I/O in flight
 *
 * Reserved bits fill in as the pager arrives (writeback, referenced,
 * reclaim, mlocked, swapbacked, ...). Width is uint32_t for headroom.
 */
enum folio_flags : uint32_t {
    FOLIO_FLAGS_NONE = 0,
    FOLIO_FLAG_ON_LRU = (1u << 0),
    FOLIO_FLAG_ANON = (1u << 1),
    FOLIO_FLAG_MAPPED = (1u << 2),
    FOLIO_FLAG_DIRTY = (1u << 3),
    FOLIO_FLAG_UPTODATE = (1u << 4),
    FOLIO_FLAG_LOCKED = (1u << 5),
};

enum folio_tag : uintptr_t {
    FOLIO_TAG_ADDRESS_SPACE = 1,
    FOLIO_TAG_ANON = 2,
};

#define FOLIO_TAG_BITS 0x7

struct folio {
    struct page *base_page;
    _Atomic enum folio_flags flags;
    uint8_t order; /* 2^order pages */
    refcount_t refcount;
    mapcount_t mapcount;
    void *mapping; /* tagged, anon_vma or file_vma_range */
    pgoff_t index;
    struct list_head lru; /* reclaim linkage */
};
uint32_t page_get_folio_index(struct page *p);
bool page_is_folio_head(struct page *p);

struct folio *folio_alloc_folio_struct();
void folio_free_folio_struct(struct folio *f);

struct folio *folio_alloc_internal(uint8_t order, enum alloc_flags flags,
                                   enum alloc_behavior bh);
void folio_free(struct folio *folio);

/* page <-> folio backptrs, give every struct page
 * folio at alloc, clear them at free */
void folio_bind_pages(struct folio *f);
void folio_unbind_pages(struct folio *f);

/* Operates on the data pages */
void folio_zero(struct folio *f);
void folio_copy(const struct folio *src, struct folio *dst);

/* LRU */
void folio_add_lru(struct folio *f);
void folio_del_lru(struct folio *f);

/* Some struct page functions here for folio */
static inline void page_set_folio(struct page *p, struct folio *f) {
    page_set_tag(p, PAGE_TAG_FOLIO);
    page_set_payload(p, (uintptr_t) f);
}

static inline struct folio *page_get_folio(const struct page *p) {
    kassert(page_get_tag(p) == PAGE_TAG_FOLIO);
    return (struct folio *) page_get_payload(p);
}

static inline void page_clear_folio(struct page *p) {
    kassert(page_get_tag(p) == PAGE_TAG_FOLIO);
    page_set_payload(p, 0);
    page_set_tag(p, PAGE_TAG_NONE);
}

static inline bool folio_mapped(const struct folio *f) {
    return atomic_load(&f->mapcount) > 0;
}

static inline unsigned long folio_nr_pages(const struct folio *f) {
    return pow2(f->order);
}

static inline void folio_set_tag(struct folio *f, enum folio_tag t) {
    f->mapping = (void *) ((uintptr_t) f->mapping & ~FOLIO_TAG_BITS);
    f->mapping = (void *) ((uintptr_t) f->mapping | t);
}

static inline enum folio_tag folio_get_tag(const struct folio *f) {
    return (enum folio_tag)((uintptr_t) f->mapping & FOLIO_TAG_BITS);
}

static inline bool folio_test_flag(const struct folio *f, enum folio_flags m) {
    return atomic_load_explicit(&f->flags, memory_order_acquire) & m;
}

static inline void folio_set_flag(struct folio *f, enum folio_flags m) {
    atomic_fetch_or_explicit(&f->flags, m, memory_order_release);
}

static inline void folio_clear_flag(struct folio *f, enum folio_flags m) {
    atomic_fetch_and_explicit(&f->flags, (enum folio_flags) ~(uint32_t) m,
                              memory_order_release);
}

static inline bool folio_test_set_flag(struct folio *f, enum folio_flags m) {
    return atomic_fetch_or_explicit(&f->flags, m, memory_order_acq_rel) & m;
}

static inline bool folio_get(struct folio *f) {
    return refcount_inc_not_zero(&f->refcount);
}

static inline void folio_put(struct folio *f) {
    if (refcount_dec_and_test(&f->refcount))
        folio_free(f);
}

/* Nth page */
static inline struct page *folio_get_page(const struct folio *f, size_t n) {
    pfn_t nth_pfn = page_get_pfn(f->base_page) + n;
    return page_for_pfn(nth_pfn);
}

static inline pfn_t folio_get_pfn(const struct folio *f) {
    return page_get_pfn(f->base_page);
}

/* reverse of folio_page(): a mapped frame -> its folio, via the page
 * backpointer. The fault/rmap paths take a PTE's phys and land back here. */
static inline struct folio *folio_from_page(const struct page *p) {
    return p ? page_get_folio(p) : NULL;
}

static inline struct folio *folio_from_paddr(paddr_t pa) {
    return folio_from_page(page_for_paddr(pa));
}

static inline paddr_t folio_get_paddr(const struct folio *f) {
    return page_get_paddr(f->base_page);
}

/* HHDM map of base */
static inline vaddr_t folio_get_vaddr(const struct folio *f) {
    return hhdm_paddr_to_vaddr(folio_get_paddr(f));
}

static inline paddr_t folio_get_paddr_for(const struct folio *f, size_t n) {
    return folio_get_paddr(f) + n * PAGE_SIZE;
}

/* HHDM map of page n */
static inline vaddr_t folio_get_vaddr_for(const struct folio *f, size_t n) {
    return hhdm_paddr_to_vaddr(folio_get_paddr_for(f, n));
}

/* state */
static inline void folio_mark_dirty(struct folio *f) {
    folio_set_flag(f, FOLIO_FLAG_DIRTY);
}

static inline void folio_mark_uptodate(struct folio *f) {
    folio_set_flag(f, FOLIO_FLAG_UPTODATE);
}

static inline void folio_set_anon(struct folio *f, struct anon_vma *av,
                                  pgoff_t index) {
    kassert(!folio_test_set_flag(f, FOLIO_FLAG_ANON));
    f->mapping = av;
    f->index = index;
    folio_set_tag(f, FOLIO_TAG_ANON);
}

static inline bool folio_is_anon(const struct folio *f) {
    /* check tag, no atomic load */
    return folio_get_tag(f) == FOLIO_TAG_ANON;
}

static inline bool folio_is_file_vma_range(const struct folio *f) {
    return folio_get_tag(f) == FOLIO_TAG_ADDRESS_SPACE;
}

static inline struct anon_vma *folio_get_anon_vma(const struct folio *f) {
    kassert(folio_is_anon(f));
    return (struct anon_vma *) ((uintptr_t) f->mapping & ~FOLIO_TAG_BITS);
}

static inline struct file_vma_range *
folio_get_file_vma_range(const struct folio *f) {
    kassert(folio_is_file_vma_range(f));
    return f->mapping;
}

static inline bool folio_trylock(struct folio *f) {
    return !folio_test_set_flag(f, FOLIO_FLAG_LOCKED);
}

static inline void folio_lock(struct folio *f) {
    while (true) {
        if (folio_trylock(f))
            break;

        while (folio_test_flag(f, FOLIO_FLAG_LOCKED))
            cpu_relax();
    }
}

static inline void folio_unlock(struct folio *f) {
    folio_clear_flag(f, FOLIO_FLAG_LOCKED);
}

/* mapcount, number of PTEs pointing at this folio */
static inline void folio_mapcount_inc(struct folio *f) {
    refcount_inc(&f->mapcount); /* Using refcount functions is fine and safe */
}

static inline bool
folio_mapcount_dec(struct folio *f) { /* true if dropped to 0 */
    return refcount_dec_and_test(&f->mapcount);
}

#define folio_for_each_page_struct(f, p)                                       \
    for (size_t __i = 0;                                                       \
         __i < folio_nr_pages(f) && ((p) = folio_get_page((f), __i), true);    \
         __i++)

#define folio_for_each_page_paddr(f, p)                                        \
    for (size_t __i = 0; __i < folio_nr_pages(f) &&                            \
                         ((p) = folio_get_paddr_for((f), __i), true);          \
         __i++)

#define folio_for_each_page_vaddr(f, p)                                        \
    for (size_t __i = 0; __i < folio_nr_pages(f) &&                            \
                         ((p) = folio_get_vaddr_for((f), __i), true);          \
         __i++)

#define folio_alloc_1(sz)                                                      \
    folio_alloc_internal((sz), ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_DEFAULT)
#define folio_alloc_2(sz, fl)                                                  \
    folio_alloc_internal((sz), (fl), ALLOC_BEHAVIOR_DEFAULT)
#define folio_alloc_3(sz, fl, bh) folio_alloc_internal((sz), (fl), (bh))
#define folio_alloc(...)                                                       \
    _DISPATCH(folio_alloc, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
