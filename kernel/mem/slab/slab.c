/* This slab allocator takes in a size, alloc_flags, and behavior.
 *
 * Depending on behavior, we are(n't) allowed to do certain things.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ All allocation paths are influenced by the specified behavior, │
 *   │ but that won't be discussed here to keep things brief.         │
 *   │ The general rule is that touching the freequeue may cause      │
 *   │ faults, and the physical memory allocator may trigger blocks.  │
 *   │ The physical memory allocator can be requested to not block.   │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * The general allocation flow is as follows:
 *
 * If the allocation does not fit in a slab, simply allocate and
 * map multiple pages to satisfy the allocation. Then, check the
 * flags and see if we're allowed to do other things. If we can
 * block/GC, then go through the freequeue and slab GC lists
 * and do a little bit of flush work if some slabs are too
 * old/there are too many elements in the freequeue. Reduce
 * the amount of flush/draining if the fast behavior is specified.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ All slabs anywhere in the slab allocator are nonmovable.       │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * If the allocation does fit in a slab, then...
 *
 * First, we determine if we MUST scale up our allocation to satisfy
 * cache alignment if cache alignment is requested.
 *
 * Second, we check if our magazine has anything. If pageable memory
 * is requested, and the magazines are very full, and the allocation
 * size is small, just take from the magazines (they are nonpageable).
 *
 * If pageable memory is requested, and the magazines are not so full,
 * and a larger size is requested, do not take from them.
 *
 * Determining whether we MUST take from our magazine depending on
 * the size of an allocation and the input memory type will be done
 * via heuristics that will scale steadily both ways.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ The goal is to make sure that pageable allocations do not steal│
 *   │ everything from nonpageable allocations from the magazines     │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * If nonpageable memory is requested, and the magazines are not empty,
 * just take memory from the magazines.
 *
 * If the magazines are not chosen for the allocation, then things get
 * a bit hairier.
 *
 * First, check if the allocation MUST be from the local node. If this
 * is the case, simply allocate from the local pageable/nonpageable slab
 * cache.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ If an allocation is pageable, then there will be a heuristic   │
 *   │ that checks whether or not there are so many things in a given │
 *   │ nonpageable cache that it is worth it to allocate from it.     │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * If the slab cache has nothing available, just map a new page to the local
 * node if the allocation MUST be from the local node.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ Slab creation uses a GC list of slabs that are going to be     │
 *   │ destroyed, so instead of constantly calling into the physical  │
 *   │ memory allocator, slabs can be reused.                         │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * Now, if the allocation does not need to come from the local node, then things
 * get real fun. Each slab domain has a zonelist for the other domains relative
 * to itself, sorted by distance. This list is traversed, and depending on slab
 * cache slab availability and physical memory availability, a cache is selected
 * for allocation. A scoring heuristic is applied to bias the result towards
 * within the selected locality. If flexible locality is selected, the
 * algo will potentially select a further node if it has high availability
 * compared to the closer nodes.
 *
 * The slab cache picking logic may be biased based on given input arguments.
 * For example, if a FAST behavior is specified, the slab cache picking logic
 * might apply a higher weight to the local cache to minimize lock contention
 * and maximize memory locality.
 *
 * Now a slab cache MUST have been selected for this allocation. If the slab
 * cache is the local node, then first try and free the freequeue to
 * the local magazines or to the slab cache until a given amount of target
 * elements is flushed from it or the freequeue becomes empty. Afterwards,
 * if there is still an unsatisfactory amount of elements in the per-cpu
 * cache/magazine (too few), allocations from the slab cache are performed.
 *
 * Just like all slab cache allocations, the same heuristics on picking
 * whether or not a nonpageable cache MUST fulfill a pageable allocation,
 * and the use of the GC list for creation of new slabs are used here.
 *
 * Finally, we MAY have a memory address to return. If we have none,
 * then we have likely ran out of memory, and so, MUST return NULL.
 *
 * If we do return NULL after the initial allocation, and a flexible
 * locality is permitted, then we just try again with a further node.
 *
 * But, we are not done yet. If the FAST behavior is not specified,
 * and if we are allowed to take faults, the slab GC list will be
 * checked to figure out what slabs MUST be fully deleted (since
 * this will free them up for use in other memory management subsystems).
 *
 * This selection operation depends on a variety of things, such as the
 * memory pressure (recent usage), recycling frequency, and other
 * heuristics about recent memory recycling usage.
 *
 * After the slab GC list has some elements truly deleted (or not, if
 * the heuristics determine that it is not necessary), we are finally
 * done, and can return the memory address that we had been anticipating
 * all along.
 *
 * The GC list uses a red-black tree ordered by slab enqueue time,
 * so the oldest slab can be picked for deletion in amortized time.
 *
 */

#include <console/printf.h>
#include <kassert.h>
#include <math/bit_ops.h>
#include <math/div.h>
#include <math/ilog2.h>
#include <math/pow.h>
#include <math/sort.h>
#include <mem/address_range.h>
#include <mem/alloc.h>
#include <mem/asan.h>
#include <mem/domain.h>
#include <mem/pmm.h>
#include <mem/simple_alloc.h>
#include <mem/slab.h>
#include <mem/vas.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stack_depot.h>
#include <static_call.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "internal.h"
#include "mem/domain/internal.h"
#include "stat_internal.h"

/* const size_t slab_class_sizes_const[] = {
    SLAB_MIN_SIZE, 16,  24,  32,  40,  48,           56,  64,  80,  96,
    112,           128, 144, 160, 176, 192,          224, 256, 320, 384,
    448,           512, 640, 768, 896, SLAB_MAX_SIZE}; */

const size_t slab_class_sizes_const[] = {
    SLAB_MIN_SIZE, 16, 32, 64, 96, 128, 192, 256, 512, SLAB_MAX_SIZE};

#define SLAB_CLASS_SIZES_CONST_COUNT                                           \
    sizeof(slab_class_sizes_const) / sizeof(*slab_class_sizes_const)

ADDRESS_RANGE_DECLARE(
    slab, .base = SLAB_HEAP_START, .size = SLAB_HEAP_END - SLAB_HEAP_START,
    .flags = ADDRESS_RANGE_STATIC,
    .page_fault_handler = &slab_page_fault_handler
    /* alignment does not need to be provided for static entries */);

struct slab_globals slab_global = {0};
LOG_HANDLE_DECLARE_DEFAULT(slab);
LOG_SITE_DECLARE_DEFAULT(slab);

/* If our cache is PAGEABLE_ZERO, we can demand page it in
 *
 * In the future, PAGEABLE will want to be demand paged too, maybe
 *
 * TODO: zero pager interaction */
static void *slab_map_new(struct slab_cache *cache,
                          paddr_t phys_out[SLAB_MAX_PAGES],
                          struct slab_chunk **out) {
    struct slab_domain *domain = cache->parent_domain;
    size_t pages = cache->pages_per_slab;
    enum slab_type type = cache->type;
    kassert(pages <= SLAB_MAX_PAGES);

    memset(phys_out, 0, pages * sizeof(paddr_t));
    size_t pages_mapped = 0;
    vaddr_t virt_base = 0x0;

    if (cache->type != SLAB_TYPE_PAGEABLE_ZERO) {
        for (size_t i = 0; i < pages; i++) {
            if (domain) {
                phys_out[i] = domain_alloc_from_domain(domain->domain, 1);
            } else {
                phys_out[i] = pmm_alloc_page();
            }
            if (unlikely(!phys_out[i]))
                goto err;
        }
    } else {
        if (domain) {
            phys_out[0] = domain_alloc_from_domain(domain->domain, 1);
        } else {
            phys_out[0] = pmm_alloc_page();
        }

        if (unlikely(!phys_out[0]))
            goto err;
    }

    virt_base = slab_chunks_alloc(&cache->chunks, out);
    if (unlikely(!virt_base))
        goto err;

    uint64_t pflags = slab_page_flags(type);

    for (pages_mapped = 0; pages_mapped < pages; pages_mapped++) {
        vaddr_t virt = virt_base + pages_mapped * PAGE_SIZE;
        if (cache->type != SLAB_TYPE_PAGEABLE_ZERO) {
            paddr_t phys = phys_out[pages_mapped];
            if (unlikely(vmm_map_page(virt, phys, pflags) < 0))
                goto err;
        } else {
            if (pages_mapped == 0) {
                if (unlikely(vmm_map_page(virt, phys_out[0], pflags) < 0))
                    goto err;
            } else {
                if (unlikely(vmm_mark_demand_page(
                                 virt, DEMAND_PAGE_FLAG_ZERO_MEMORY |
                                           DEMAND_PAGE_FLAG_WRITABLE) < 0))
                    goto err;
            }
        }
    }

    return (void *) virt_base;

err:
    for (size_t i = 0; i < pages; i++) {
        paddr_t phys = phys_out[i];
        pmm_free_page(phys);
    }

    for (size_t i = 0; i < pages_mapped; i++) {
        vaddr_t virt = virt_base + i * PAGE_SIZE;
        vmm_unmap_page(virt);
    }

    if (virt_base)
        slab_chunks_free(&cache->chunks, *out, virt_base);

    return NULL;
}

static void slab_free_virt_and_phys(struct slab *slab) {

#ifdef DEBUG_SLAB_DEEP
    /* First drop references to all stack_handle_t's */
    for (size_t i = 0; i < slab->parent_cache->objs_per_slab; i++) {

        /* Might not necessarily exist */
        if (slab->traces[i])
            stack_depot_put(slab->traces[i]);
    }
#endif

    vaddr_t virt_base = (vaddr_t) slab;
    struct slab_chunk *chunk = slab->parent_chunk;
    /* Free through chunk's owner, as a GC reused slab's parent_cache may
     * differ from the cache whose chunk it physically lives in */
    struct slab_chunks *chunks = chunk->owner;

    for (size_t i = 0; i < slab->page_count; i++) {
        size_t virt = virt_base + i * PAGE_SIZE;
        if (slab->backing_pages[i]) {
            paddr_t phys = page_get_paddr(slab->backing_pages[i]);
            pmm_free_page(phys);
        }

        vmm_unmap_page(virt);
    }

    slab_chunks_free(chunks, chunk, virt_base);
}

void slab_cache_init(size_t order, struct slab_cache *cache,
                     struct slab_size_constant *ssc) {
    cache->order = order;
    cache->obj_size = ssc->size;
    cache->obj_align = ssc->align;
    cache->obj_stride = SLAB_ALIGN_UP(ssc->size, ssc->align);
    cache->pages_per_slab = ssc->internal.cand.pages;
    size_t page_ptr_size = sizeof(struct page *) * cache->pages_per_slab;

    uint64_t available = NON_SLAB_SPACE(cache);

    if (cache->obj_size > available)
        panic("Slab class too large, object size is %u with %u available "
              "bytes -- insufficient",
              cache->obj_size, available);

    uint64_t n;
    for (n = NON_SLAB_SPACE(cache) / ssc->size; n > 0; n--) {
        uint64_t bitmap_bytes = SLAB_BITMAP_BYTES_FOR(n);
        uintptr_t data_start = sizeof(struct slab) + bitmap_bytes +
                               page_ptr_size + sizeof(stack_handle_t) * n;
        data_start = SLAB_ALIGN_UP(data_start, ssc->align);
        uintptr_t data_end = data_start + n * cache->obj_stride;

        if (data_end <= PAGE_SIZE * cache->pages_per_slab)
            break;
    }

    spinlock_init(&cache->lock);
    cache->objs_per_slab = n;

    if (cache->objs_per_slab == 0)
        panic("Slab cache cannot hold any objects per slab!");

    cache->bitmap_bytes = SLAB_BITMAP_BYTES_FOR(cache->objs_per_slab);

#ifdef DEBUG_SLAB_DEEP
    size_t traces_size = sizeof(stack_handle_t) * cache->objs_per_slab;
#else
    size_t traces_size = 0;
#endif

    cache->slab_metadata_size =
        sizeof(struct slab) + cache->bitmap_bytes + page_ptr_size + traces_size;
    cache->slab_metadata_size =
        ALIGN_UP(cache->slab_metadata_size, sizeof(uint64_t));

    kassert(cache->objs_per_slab * cache->obj_stride +
                cache->slab_metadata_size <=
            cache->pages_per_slab * PAGE_SIZE);

    /* We must hold this true because right now we only handle
     * cases where the slab metadata is just 1 page. If we want,
     * in the future if we somehow get GARGANTUAN slabs we can
     * handle cases where this is more than one page. But I'll
     * leave that as an optional TODO: */
    kassert(cache->slab_metadata_size <= PAGE_SIZE);

    INIT_LIST_HEAD(&cache->slabs[SLAB_FREE]);
    INIT_LIST_HEAD(&cache->slabs[SLAB_PARTIAL]);
    INIT_LIST_HEAD(&cache->slabs[SLAB_FULL]);
    slab_chunks_init(&cache->chunks, cache);
}

static void slab_zero_out(struct slab *slab, size_t n_pages) {
    void *start = (void *) slab->mem;
    size_t non_data = slab->mem - (vaddr_t) slab;
    kassert(n_pages);
    size_t len = n_pages * PAGE_SIZE - non_data;
    memset(start, 0, len);
}

struct slab *slab_init(struct slab *slab, struct slab_cache *parent) {
    void *page = slab;

#ifdef DEBUG_ASAN
    /* We must unpoison the slab because if it's taken off GC,
     * the slab might still be poisoned according to the previous
     * use, which means previously bitmap bytes would be different metadata
     * and we could touch it and cause ASAN to trip */
    asan_unpoison(page, parent->slab_metadata_size);
#endif

    slab->parent_cache = parent;
    slab->bitmap = slab_get_bitmap_location(slab);

#ifdef DEBUG_SLAB_DEEP
    slab->traces = slab_get_traces_location(slab);
#endif

    vaddr_t data_start = (vaddr_t) page + parent->slab_metadata_size;
    data_start = SLAB_ALIGN_UP(data_start, parent->obj_align);
    slab->mem = data_start;

    slab->used = 0;
    slab->state = SLAB_FREE;
    slab->gc_enqueue_time_ms = 0;
    slab->type = parent->type;
    rbt_init_node(&slab->rb);
    INIT_LIST_HEAD(&slab->list);
    memset(slab->bitmap, 0, parent->bitmap_bytes);

    /* Pre-set unused trailing bits */
    size_t total_bits = parent->bitmap_bytes * 8;
    for (size_t b = parent->objs_per_slab; b < total_bits; b++) {
        size_t byte_idx = b / 8;
        slab->bitmap[byte_idx] |= (uint8_t) (1U << (b % 8));
    }

#ifdef DEBUG_SLAB_DEEP
    memset(slab->traces, 0, parent->objs_per_slab * sizeof(stack_handle_t));
#endif

    if (slab->type == SLAB_TYPE_NONPAGEABLE_ZERO) {
        slab_zero_out(slab, parent->pages_per_slab);
    } else if (slab->type == SLAB_TYPE_PAGEABLE_ZERO) {
        /* TODO: handle cases where the other pages are mapped
         * once zero pager infrastructure comes alive */
        slab_zero_out(slab, 1); /* Just the first page */
    }

    return slab;
}

static struct slab *slab_create_new(struct slab_cache *cache) {
    paddr_t phys[SLAB_MAX_PAGES];
    struct slab_chunk *out;
    void *page = slab_map_new(cache, phys, &out);
    if (!page)
        return NULL;

#ifdef DEBUG_ASAN
    /* Make sure we can touch the metadata */
    asan_unpoison(page, cache->slab_metadata_size);
#endif

    struct slab *slab = (struct slab *) page;
    slab->parent_chunk = out;
    slab->page_count = cache->pages_per_slab;

    for (size_t i = 0; i < cache->pages_per_slab; i++) {
        if (phys[i]) {
            slab->backing_pages[i] = page_for_paddr(phys[i]);
        } else {
            slab->backing_pages[i] = NULL;
        }
    }

    return slab_init(slab, cache);
}

/* First we try and steal a slab from the GC list.
 * If this does not work, we will map a new one. */
struct slab *slab_create(struct slab_cache *cache,
                         enum alloc_behavior behavior) {
    struct slab *slab = NULL;
    struct slab_domain *local = slab_domain_local();
    kassert(cache->type != SLAB_TYPE_NONE);

    /* This is only searched if we are allowed to fault -
     * iteration through GC slabs may touch pageable slabs and
     * trigger a page fault, so we must be careful here */
    if (alloc_behavior_may_fault(behavior))
        slab = slab_gc_get_for_cache(cache);

    if (slab) {
        if (slab_resize(slab, cache->pages_per_slab)) {
            slab_stat_gc_object_reclaimed(local);
            return slab_init(slab, cache);
        } else {
            /* Came out of cache's domain GC, so must go back to it */
            slab_gc_enqueue(cache->parent_domain, slab);
            slab = NULL;
        }
    }

    if (behavior & SLAB_ALLOC_BEHAVIOR_FROM_ALLOC) {
        slab = slab_create_new(cache);

        if (slab && cache->parent_domain == local)
            slab_stat_alloc_new_slab(local);

        if (slab && cache->parent_domain != local)
            slab_stat_alloc_new_remote_slab(local);
    }

    return slab;
}
static void *slab_alloc_from(struct slab_cache *cache, stack_handle_t handle,
                             struct slab *slab) {
    slab_check_assert(slab);

    SPINLOCK_ASSERT_HELD(&cache->lock);
    kassert(slab->state != SLAB_FULL);

    uint64_t *bm = (uint64_t *) slab->bitmap;
    size_t nwords = DIV_ROUND_UP(cache->objs_per_slab, 64);

    for (size_t w = 0; w < nwords; w++) {
        if (bm[w] != UINT64_MAX) {
            uint64_t free_bits = ~bm[w];
            uint64_t bit = __builtin_ctzll(free_bits);
            uint64_t i = w * 64 + bit;

            if (i >= cache->objs_per_slab)
                break;

            SLAB_BITMAP_SET(bm[w], 1ULL << bit);
            slab->used++;

#ifdef DEBUG_SLAB_DEEP
            kassert(&slab->traces[i] < (stack_handle_t *) slab->mem);
            if (slab->traces[i])
                stack_depot_put(slab->traces[i]);

            slab->traces[i] = handle;
#endif

            if (slab->used == cache->objs_per_slab) {
                slab_move(cache, slab, SLAB_FULL);
            } else if (slab->used == 1) {
                slab_move(cache, slab, SLAB_PARTIAL);
            }

            vaddr_t ret = slab->mem + i * cache->obj_stride;
            kassert(ret > (vaddr_t) slab &&
                    ret < (vaddr_t) slab + cache->pages_per_slab * PAGE_SIZE);

            slab_check_assert(slab);
            return (void *) ret;
        }
    }

    slab_check_assert(slab);
    return NULL;
}

void slab_destroy(struct slab *slab) {
    slab_list_del(slab);
    slab_free_virt_and_phys(slab);
}

static void slab_bitmap_free(struct slab *slab, void *obj) {
    slab_check_assert(slab);

    uint64_t byte_idx;
    uint8_t bit_mask;
    slab_index_and_mask(slab, obj, &byte_idx, &bit_mask);

    if (!SLAB_BITMAP_TEST(slab->bitmap[byte_idx], bit_mask)) {
        panic("Possible UAF of addr %p for bitmap 0b%b with bitmask 0b%b", obj,
              slab->bitmap[byte_idx], bit_mask);
    }

    SLAB_BITMAP_UNSET(slab->bitmap[byte_idx], bit_mask);
    slab->used -= 1;
}

void slab_free_old(struct slab *slab, void *obj) {
    struct slab_cache *cache = slab->parent_cache;

    enum irql slab_cache_irql = spin_lock(&cache->lock);

    slab_bitmap_free(slab, obj);

    if (slab->used == 0) {
        slab_move(cache, slab, SLAB_FREE);
        if (slab_should_enqueue_gc(slab)) {
            slab_list_del(slab);
            spin_unlock(&cache->lock, slab_cache_irql);

            slab_free_virt_and_phys(slab);
            return;
        }
    } else if (slab->state == SLAB_FULL) {
        slab_move(cache, slab, SLAB_PARTIAL);
    }

    slab_check_assert(slab);
    spin_unlock(&cache->lock, slab_cache_irql);
}

static void *slab_try_alloc_from_slab_list(struct slab_cache *cache,
                                           stack_handle_t handle,
                                           struct list_head *list) {
    SPINLOCK_ASSERT_HELD(&cache->lock);
    struct list_head *node, *temp;
    struct slab *slab;
    void *ret = NULL;

    /* This should never iterate more than once */
    list_for_each_safe(node, temp, list) {
        slab = slab_from_list_node(node);
        ret = slab_alloc_from(cache, handle, slab);
        if (ret)
            goto out;
    }

out:
    return ret;
}

void slab_cache_insert(struct slab_cache *cache, struct slab *slab) {
    enum irql irql = spin_lock(&cache->lock);

    slab_init(slab, cache);
    slab_list_add(cache, slab);

    spin_unlock(&cache->lock, irql);
}

void *slab_cache_try_alloc_from_lists(struct slab_cache *c,
                                      stack_handle_t handle) {
    SPINLOCK_ASSERT_HELD(&c->lock);

    void *ret =
        slab_try_alloc_from_slab_list(c, handle, &c->slabs[SLAB_PARTIAL]);
    if (ret)
        return ret;

    return slab_try_alloc_from_slab_list(c, handle, &c->slabs[SLAB_FREE]);
}

void *slab_alloc_old(struct slab_cache *cache) {
    void *ret = NULL;

    enum irql irql = spin_lock(&cache->lock);
    ret = slab_cache_try_alloc_from_lists(cache, /*handle=*/NULL);
    spin_unlock(&cache->lock, irql);
    if (ret)
        goto out;

    struct slab *slab;
    slab = slab_create_new(cache);
    if (!slab)
        goto out;

    irql = spin_lock(&cache->lock);
    slab_list_add(cache, slab);
    ret = slab_alloc_from(cache, /*handle=*/NULL, slab);
    spin_unlock(&cache->lock, irql);

out:
    return ret;
}

int32_t slab_size_to_index(size_t size) {
    size_t lo = 0;
    size_t hi = slab_global.num_sizes;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (slab_global.class_sizes[mid].size >= size) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    if (lo >= slab_global.num_sizes)
        return -1;

    return (int32_t) lo;
}

static inline bool kmalloc_size_fits_in_slab(size_t size) {
    return slab_size_to_index(size) >= 0;
}

static int slab_class_sort_cmp(const void *a, const void *b) {
    const struct slab_size_constant *sa = a;
    const struct slab_size_constant *sb = b;

    if (sa->size < sb->size)
        return -1;
    if (sa->size > sb->size)
        return 1;

    if (sa->align > sb->align)
        return -1;
    if (sa->align < sb->align)
        return 1;

    return 0;
}

static void log_dupes(const char *keep, const char *discard, size_t size) {
    slab_info("Sizes of slab cache %s and %s are the same (%zu), ignoring %s, "
              "keeping %s",
              discard, keep, size, discard, keep);
}

void slab_allocator_init() {
    /* bootstrap VAS */
    slab_global.vas = vas_bootstrap(SLAB_HEAP_START, SLAB_HEAP_END);
    if (!slab_global.vas)
        panic("Could not initialize slab VAS");
    struct slab_size_constant *start = __skernel_slab_sizes;
    struct slab_size_constant *end = __ekernel_slab_sizes;
    size_t dyn_count = end - start;
    size_t total_input = dyn_count + SLAB_CLASS_SIZES_CONST_COUNT;

    struct slab_size_constant *staging =
        simple_alloc(slab_global.vas, total_input * sizeof(*staging));
    kassert(staging);
    slab_order_map_init();

    size_t sidx = 0;
    /* "Constant" ones */
    for (size_t i = 0; i < SLAB_CLASS_SIZES_CONST_COUNT; i++) {
        staging[sidx++] = (struct slab_size_constant){
            .name = "default slab size",
            .size = slab_class_sizes_const[i],
            .align = SLAB_OBJ_ALIGN_DEFAULT,
        };
    }
    /* "Dynamic" ones */
    for (struct slab_size_constant *ssc = start; ssc < end; ssc++) {
        staging[sidx++] = (struct slab_size_constant){
            .name = ssc->name,
            .size = ssc->size,
            .align = ssc->align,
        };
    }
    kassert(sidx == total_input);

    qsort(staging, total_input, sizeof(*staging), slab_class_sort_cmp);

    struct slab_size_constant *tmp =
        simple_alloc(slab_global.vas, total_input * sizeof(*tmp));
    kassert(tmp);

    size_t out = 0;
    for (size_t i = 0; i < total_input; i++) {
        if (out == 0 || staging[i].size != tmp[out - 1].size) {
            tmp[out++] = staging[i];
        } else {
            log_dupes(tmp[out - 1].name, staging[i].name, staging[i].size);
        }
    }

    simple_free(slab_global.vas, staging, total_input * sizeof(*staging));

    slab_global.num_sizes = out;
    slab_global.class_sizes =
        simple_alloc(slab_global.vas,
                     slab_global.num_sizes * sizeof(*slab_global.class_sizes));
    kassert(slab_global.class_sizes);
    memcpy(slab_global.class_sizes, tmp,
           slab_global.num_sizes * sizeof(*slab_global.class_sizes));
    slab_global.caches.caches = slab_caches_alloc();
    for (uint64_t i = 0; i < slab_global.num_sizes; i++) {
        struct slab_size_constant *ssc = &slab_global.class_sizes[i];
        struct slab_cache *sc = &slab_global.caches.caches[i];
        ssc->internal.cand = slab_elcm(ssc->size, ssc->align);
        slab_cache_init(i, sc, ssc);
        sc->parent_domain = NULL;
        sc->type = SLAB_TYPE_NONPAGEABLE;
        sc->parent = &slab_global.caches;
        slab_info("Slab cache s=%u a=%u \"%s\", o=%u, p=%zu",
                  slab_global.class_sizes[i].size,
                  slab_global.class_sizes[i].align,
                  slab_global.class_sizes[i].name,
                  slab_global.caches.caches[i].objs_per_slab,
                  slab_global.class_sizes[i].internal.cand.pages);
    }

    simple_free(slab_global.vas, tmp, total_input * sizeof(*tmp));
    slab_elcm_initialize();
}

struct slab *slab_for_ptr(void *ptr) {
    kassert(slab_ptr_in_slab(ptr));
    vaddr_t vp = (vaddr_t) ptr;
    uint8_t pow2_order = slab_order_map_get(vp);
    kassert(pow2_order != SLAB_POW2_ORDER_EMPTY);

    size_t align = ipow(2, pow2_order) * PAGE_SIZE;
    return (struct slab *) ALIGN_DOWN(vp, align);
}

size_t ksize(void *ptr) {
    if (!ptr)
        return 0;

    bool in_slab = kmalloc_ptr_in_slab_validate(ptr);

    if (!in_slab) {
        struct slab_page_hdr *hdr = slab_page_hdr_for_addr(ptr);
        kassert(hdr->magic == KMALLOC_PAGE_MAGIC, "header is 0x%lx",
                hdr->magic);
        return hdr->pages * PAGE_SIZE - sizeof(struct slab_page_hdr);
    }

    return slab_for_ptr(ptr)->parent_cache->obj_size;
}

size_t slab_allocation_size(vaddr_t addr) {
    return ksize((void *) addr);
}

void *kmalloc_pages_raw(struct slab_domain *parent, stack_handle_t handle,
                        size_t size, enum alloc_flags flags,
                        enum alloc_behavior behavior) {
    uint64_t total_size = size + sizeof(struct slab_page_hdr);
    uint64_t pages = PAGES_NEEDED_FOR(total_size);

    void *vptr;
    if (flags & ALLOC_FLAG_PAGEABLE) {
        vptr = page_alloc_demand(pages, flags, behavior);
    } else {
        vptr = page_alloc(pages, flags, behavior);
        if (vptr && (flags & ALLOC_FLAG_ZERO_ON_ALLOC))
            memset(vptr, 0, total_size);
    }

    if (!vptr)
        return NULL;

    struct slab_page_hdr *hdr = (struct slab_page_hdr *) vptr;
    hdr->magic = KMALLOC_PAGE_MAGIC;
    hdr->pages = pages;
    hdr->domain = parent;
    hdr->pageable = (flags & ALLOC_FLAG_PAGEABLE);
    hdr->handle = handle;

    return (void *) (hdr + 1);
}

static void *kmalloc_old(size_t size, enum alloc_flags flags) {
    if (size == 0)
        return NULL;

    int idx = slab_size_to_index(size);

    void *ptr;
    if (kmalloc_size_fits_in_slab(size) &&
        slab_global.caches.caches[idx].objs_per_slab > 0) {
        ptr = slab_alloc_old(&slab_global.caches.caches[idx]);
    } else {
        /* we say NULL and just free these to domain 0 */
        ptr = kmalloc_pages_raw(NULL, NULL, size, ALLOC_FLAGS_DEFAULT,
                                ALLOC_BEHAVIOR_NORMAL);
    }

    if ((flags & ALLOC_FLAG_ZERO_ON_ALLOC) && ptr)
        memset(ptr, 0, size);

    return ptr;
}

void slab_free_page_hdr(struct slab_page_hdr *hdr, enum alloc_behavior bh) {
    uint32_t pages = hdr->pages;
    if (hdr->handle)
        stack_depot_put(hdr->handle);

    page_free(hdr, pages, bh);
}

void slab_free_addr_to_cache(void *addr, enum alloc_behavior bh) {
    kmalloc_ptr_in_slab_validate(addr);

    struct slab_page_hdr *hdr_candidate = slab_page_hdr_for_addr(addr);
    if (hdr_candidate->magic == KMALLOC_PAGE_MAGIC)
        return slab_free_page_hdr(hdr_candidate, bh);

    struct slab *slab = slab_for_ptr(addr);
    if (!slab)
        panic("Likely double free of address %p", addr);

    slab_free_old(slab, addr);
}

void kfree_old(void *ptr) {
    slab_free_addr_to_cache(ptr, ALLOC_BEHAVIOR_NORMAL);
}

void *kmalloc_pages(struct slab_domain *domain, stack_handle_t handle,
                    size_t size, enum alloc_flags flags,
                    enum alloc_behavior behavior) {
    void *ret = kmalloc_pages_raw(domain, handle, size, flags, behavior);

    if (alloc_behavior_may_fault(behavior) &&
        !alloc_behavior_is_fast(behavior)) {
        struct slab_domain *local = slab_domain_local();
        struct slab_percpu_cache *pcpu = slab_percpu_cache_local();

        /* Scale down this free_queue drain target */
        size_t pct = SLAB_FREE_QUEUE_ALLOC_PCT;
        size_t target = slab_free_queue_get_target_drain(local, pct);
        target /= 2;

        slab_free_queue_drain(pcpu, &local->free_queue, target, behavior);
    }

    if (ret)
        slab_stat_alloc_page_hit(domain);

    return ret;
}

size_t slab_allocation_index(struct slab *slab, void *ptr) {
    size_t delta = (size_t) ((uint8_t *) ptr - (uint8_t *) slab->mem);
    return delta / slab->parent_cache->obj_stride;
}

void *kmalloc_try_from_magazine(struct slab_domain *domain,
                                struct slab_percpu_cache *pcpu,
                                stack_handle_t handle, size_t size,
                                enum alloc_flags flags) {
    enum slab_magazine_type mtype = (flags & ALLOC_FLAG_ZERO_ON_ALLOC)
                                        ? SLAB_MAGAZINE_ZERO
                                        : SLAB_MAGAZINE_NORMAL;
    size_t class_idx = slab_size_to_index(size);
    struct slab_magazine *mag = &pcpu->mags[mtype][class_idx];

    /* Reserve SLAB_MAG_WATERMARK_PCT% entries for nonpageable requests */
    if (flags & ALLOC_FLAG_PAGEABLE && mag->count < SLAB_MAG_WATERMARK)
        return NULL;

    void *ret = (void *) slab_magazine_pop(mag);
    if (ret) {
        struct slab *slab = slab_for_ptr(ret);
        slab_stat_alloc_magazine_hit(domain);
        uint64_t byte_idx;
        uint8_t bit_mask;
        slab_index_and_mask(slab, ret, &byte_idx, &bit_mask);
        kassert(
            SLAB_BITMAP_TEST(slab_for_ptr(ret)->bitmap[byte_idx], bit_mask));

#ifdef DEBUG_SLAB_DEEP
        size_t idx = slab_allocation_index(slab, ret);
        if (slab->traces[idx])
            stack_depot_put(slab->traces[idx]);

        slab->traces[idx] = handle;
#endif

#ifdef DEBUG_SLAB
        if (mtype == SLAB_MAGAZINE_ZERO &&
            !is_buffer_uniform(ret, mag->obj_size, 0)) {
#ifdef DEBUG_SLAB_DEEP
            slab_dump_corruption(ret, mag, 0);
#endif
            panic("buffer size %zu idx %zu not uniform", size, class_idx);
        }
#endif
    }

    return ret;
}

static size_t slab_free_queue_drain_on_alloc(struct slab_domain *dom,
                                             struct slab_percpu_cache *c,
                                             enum alloc_behavior behavior,
                                             size_t pct) {
    if (!alloc_behavior_may_fault(behavior))
        return 0;

    /* drain a tiny bit back into our magazine */
    return slab_free_queue_drain_limited(c, dom, pct, behavior);
}

static inline size_t slab_get_search_dist(struct slab_domain *dom,
                                          uint8_t locality) {
    /* The higher the locality, the closer it is, and the less we will search */
    uint8_t numerator = ALLOC_LOCALITY_MAX - locality;
    size_t ret = dom->zonelist_entry_count * numerator / ALLOC_LOCALITY_MAX;
    if (ret == 0)
        ret = 1;

    return ret;
}

static size_t slab_cache_usable(struct slab_cache *cache) {
    size_t part = SLAB_CACHE_COUNT_FOR(cache, SLAB_PARTIAL);
    size_t free = SLAB_CACHE_COUNT_FOR(cache, SLAB_FREE);
    return part + free;
}

static int32_t slab_score_cache(struct slab_cache_ref *ref,
                                struct slab_cache *cache, bool flexible) {
    int32_t dist_weight = flexible ? SLAB_CACHE_FLEXIBLE_DISTANCE_WEIGHT
                                   : SLAB_CACHE_DISTANCE_WEIGHT;

    int32_t usable = slab_cache_usable(cache);
    int32_t dist_part = ref->locality * dist_weight;

    /* Lower is better */
    return dist_part - usable;
}

struct slab_cache *slab_search_for_cache(struct slab_domain *dom,
                                         enum alloc_flags flags, size_t size) {
    size_t idx = slab_size_to_index(size);
    uint8_t locality = ALLOC_LOCALITY_FROM_FLAGS(flags);

    bool pageable = flags & ALLOC_FLAG_PAGEABLE;
    bool flexible = flags & ALLOC_FLAG_FLEXIBLE_LOCALITY;
    bool zero = flags & ALLOC_FLAG_ZERO_ON_ALLOC;

    size_t search_distance = slab_get_search_dist(dom, locality);

    int32_t best_score = INT32_MAX;
    struct slab_cache *ret = NULL;

    enum slab_type p_type = zero ? SLAB_TYPE_PAGEABLE_ZERO : SLAB_TYPE_PAGEABLE;
    enum slab_type np_type =
        zero ? SLAB_TYPE_NONPAGEABLE_ZERO : SLAB_TYPE_NONPAGEABLE;
    for (size_t i = 0; i < search_distance; i++) {
        /* pageable, nonpageable candidates */
        struct slab_cache_ref *p_ref = &dom->zonelists[p_type].entries[i];
        struct slab_cache_ref *np_ref = &dom->zonelists[np_type].entries[i];
        struct slab_cache *p_cache = &p_ref->caches->caches[idx];
        struct slab_cache *np_cache = &np_ref->caches->caches[idx];

        int32_t p_score = slab_score_cache(p_ref, p_cache, flexible);
        int32_t np_score = slab_score_cache(np_ref, np_cache, flexible);

        size_t p_usable = slab_cache_usable(p_cache);
        size_t np_usable = slab_cache_usable(np_cache);

        /* We prevent these caches from being selected if they come up
         * empty handed -- this first loop scores based on slab usability */
        if (p_usable == 0)
            p_score = INT32_MAX;

        if (np_usable == 0)
            np_score = INT32_MAX;

        if (!pageable && np_score < best_score) {
            best_score = np_score;
            ret = np_cache;
        } else if (pageable && np_score <= p_score / 2 &&
                   np_score < best_score) {
            best_score = np_score;
            ret = np_cache;
        } else if (pageable && p_score < best_score) {
            best_score = p_score;
            ret = p_cache;
        }
    }

    if (ret)
        return ret;

    /* saying pages = 1 here is actually fine because we won't do contiguous
     * allocations. the only risk is for multi-page slabs, but those will just
     * OOM in the worst case scenario, and besides, this is a "racy
     * heuristic", so accuracy doesn't matter, if we OOM, bigger problems exist
     */
    struct domain *d = domain_alloc_pick_best_domain(dom->domain, /*pages=*/1,
                                                     search_distance, flexible);

    struct slab_domain *sd = global.domains[d->id]->slab_domain;
    struct slab_caches *sc =
        pageable ? sd->caches[p_type] : sd->caches[np_type];

    ret = &sc->caches[idx];

    return ret;
}

void slab_stat_alloc_from_cache(struct slab_cache *cache) {
    struct slab_domain *local = slab_domain_local();
    if (cache->parent_domain == local) {
        slab_stat_alloc_local_hit(local);
    } else {
        slab_stat_alloc_remote_hit(local);
    }
}

void *slab_alloc(struct slab_cache *cache, stack_handle_t handle,
                 enum alloc_behavior behavior) {
    void *ret = NULL;
    bool from_alloc = behavior & SLAB_ALLOC_BEHAVIOR_FROM_ALLOC;

    if (!alloc_behavior_may_fault(behavior) && slab_cache_is_pageable(cache))
        panic("picked pageable cache with non-fault tolerant behavior");

    enum irql irql = spin_lock(&cache->lock);

    /* First try from lists */
    ret = slab_cache_try_alloc_from_lists(cache, handle);
    if (ret) {
        if (from_alloc)
            slab_stat_alloc_from_cache(cache);
        goto out;
    }

    /* Drop the lock since we are going to do the expensive thing */
    spin_unlock(&cache->lock, irql);

    struct slab *slab = slab_create(cache, behavior);

    irql = spin_lock(&cache->lock);

    if (!slab)
        goto out;

    slab_list_add(cache, slab);
    ret = slab_alloc_from(cache, handle, slab);

out:
    spin_unlock(&cache->lock, irql);

#if DEBUG_SLAB
    if (ret && (cache->type == SLAB_TYPE_NONPAGEABLE_ZERO ||
                cache->type == SLAB_TYPE_PAGEABLE_ZERO)) {
        if (!is_buffer_uniform(ret, cache->obj_size, 0)) {
#ifdef DEBUG_SLAB_DEEP
            slab_dump_corruption(ret, NULL, cache->obj_size);
#endif
            const uint8_t *bb = (const uint8_t *) ret;
            size_t bad = 0;
            for (size_t i = 0; i < cache->obj_size; i++)
                if (bb[i]) {
                    bad = i;
                    break;
                }

            struct slab *dslab = slab_for_ptr(ret);
            vaddr_t soff = (vaddr_t) ret - (vaddr_t) dslab;
            size_t obj_idx = ((vaddr_t) ret - dslab->mem) / cache->obj_stride;

            slab_err(
                "NON-UNIFORM ZERO BUF: sz=%zu type=%d ppslab=%zu stride=%u",
                cache->obj_size, (int) cache->type, cache->pages_per_slab,
                cache->obj_stride);
            slab_err("  ret=%p slab=%p slaboff=%zu page=%zu objidx=%zu used=%u "
                     "badoff=%zu badval=0x%02x",
                     ret, (void *) dslab, (size_t) soff,
                     (size_t) (soff / PAGE_SIZE), obj_idx, dslab->used, bad,
                     bb[bad]);

            for (size_t i = 0; i < cache->obj_size; i += 16) {
                uint8_t r[16] = {0};
                char ascii[17];
                size_t n = cache->obj_size - i;
                if (n > 16)
                    n = 16;
                for (size_t j = 0; j < 16; j++) {
                    if (j < n) {
                        r[j] = bb[i + j];
                        ascii[j] =
                            (r[j] >= 0x20 && r[j] < 0x7f) ? (char) r[j] : '.';
                    } else {
                        ascii[j] = ' ';
                    }
                }
                ascii[16] = '\0';
                slab_err("  +%03zu: %02x %02x %02x %02x %02x %02x %02x %02x  "
                         "%02x %02x %02x %02x %02x %02x %02x %02x  |%s|",
                         i, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                         r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15],
                         ascii);
            }

            panic("buffer size %zu not uniform", cache->obj_size);
        }
    }
#endif

    return ret;
}

void *slab_alloc_retry(struct slab_domain *domain, stack_handle_t handle,
                       size_t size, enum alloc_flags flags,
                       enum alloc_behavior behavior) {
    /* here we run emergency GC to try and reclaim a little memory */
    enum slab_gc_flags gc_flags = SLAB_GC_FLAG_AGG_EMERGENCY;

    /* setup our flags */
    if (!kmalloc_size_fits_in_slab(size)) {
        gc_flags |= SLAB_GC_FLAG_FORCE_DESTROY;
        size_t needed = PAGES_NEEDED_FOR(size);

        if (needed > SLAB_GC_FLAG_DESTROY_TARGET_MAX)
            needed = SLAB_GC_FLAG_DESTROY_TARGET_MAX;

        SLAB_GC_FLAG_DESTROY_TARGET_SET(gc_flags, needed);
    } else {
        size_t order = slab_size_to_index(size);
        SLAB_GC_FLAG_ORDER_BIAS_SET(gc_flags, order);
    }

    /* here we go! run GC for the appropriate domain */
    slab_gc_run(&domain->slab_gc, gc_flags);

    /* ok now we have ran the emergency GC, let's try again... */
    if (!kmalloc_size_fits_in_slab(size)) {
        /* here, `domain` should be the local domain... */
        return kmalloc_pages(domain, handle, size, flags, behavior);
    } else {
        /* here, `domain` might be another domain */

        bool zero = flags & ALLOC_FLAG_ZERO_ON_ALLOC;
        enum slab_type p_type =
            zero ? SLAB_TYPE_PAGEABLE_ZERO : SLAB_TYPE_PAGEABLE;
        enum slab_type np_type =
            zero ? SLAB_TYPE_NONPAGEABLE_ZERO : SLAB_TYPE_NONPAGEABLE;

        struct slab_caches *cs = flags & ALLOC_FLAG_PAGEABLE
                                     ? domain->caches[p_type]
                                     : domain->caches[np_type];

        struct slab_cache *cache = &cs->caches[slab_size_to_index(size)];

        return slab_alloc(cache, handle,
                          behavior | SLAB_ALLOC_BEHAVIOR_FROM_ALLOC);
    }
}

void *kmalloc_new(size_t size, enum alloc_flags flags,
                  enum alloc_behavior behavior) {
    kmalloc_validate_params(size, flags, behavior);
    void *ret = NULL;
    enum irql outer = irql_raise(IRQL_DISPATCH_LEVEL);

    struct slab_domain *local_dom = slab_domain_local();
    struct slab_percpu_cache *pcpu = slab_percpu_cache_local();
    struct slab_domain *selected_dom = local_dom;

#ifdef DEBUG_SLAB_DEEP
    stack_handle_t handle = kassert(stack_depot_save_current());
#else
    stack_handle_t handle = NULL;
#endif

    slab_stat_alloc_call(local_dom);

    /* this has its own path */
    if (!kmalloc_size_fits_in_slab(size)) {
        ret = kmalloc_pages(local_dom, handle, size, flags, behavior);
        goto exit;
    }

    /* alloc fits in slab - TODO: scale size if cache alignment is requested */
    ret = kmalloc_try_from_magazine(local_dom, pcpu, handle, size, flags);

    /* if the mag alloc fails, drain our full portion of the freequeue */
    size_t pct = ret ? SLAB_FREE_QUEUE_ALLOC_PCT : 100;
    size_t drained =
        slab_free_queue_drain_on_alloc(local_dom, pcpu, behavior, pct);

    /* did the initial allocation fail but we drained something? go again... */
    if (!ret && drained) {
        ret = kmalloc_try_from_magazine(local_dom, pcpu, handle, size, flags);
    }

    /* found something -- all done, this is the fastpath.
     * we don't bother with GC or any funny stuff. */
    if (ret)
        goto exit;

    /* ok the magazine is empty and we also didn't successfully drain
     * any freequeue elements to reuse, so we now want to start searching
     * slab caches to allocate from a slab that may or may not be local */
    struct slab_cache *cache = slab_search_for_cache(local_dom, flags, size);

    /* now we have picked a slab cache - it may or may not have free slabs but
     * we definitely know where we want to get memory from now */
    selected_dom = cache->parent_domain;

    /* allocate from an existing slab or pull from the GC lists
     * or call into the physical memory allocator to get a new slab */
    ret = slab_alloc(cache, handle, behavior | SLAB_ALLOC_BEHAVIOR_FROM_ALLOC);

    /* slowpath - let's try and fill up our percpu caches so we don't
     * end up in this slowpath over and over again... */
    slab_percpu_refill(local_dom, pcpu, flags, behavior);

    /* uh oh... we found NOTHING...
     * try one last time - this will run emergency GC */
    if (unlikely(!ret) && !alloc_behavior_is_fast(behavior))
        ret = slab_alloc_retry(selected_dom, handle, size, flags, behavior);

exit:

    /* only hit if there is truly nothing left */
    if (unlikely(!ret))
        slab_stat_alloc_failure(local_dom);

    if (ret)
        asan_alloc(ret, size, ksize(ret));

    irql_lower(outer);
    return ret;
}

void *kmalloc_from_domain(domain_id_t domain, size_t size) {
    size_t index = slab_size_to_index(size);
    struct slab_caches *cs =
        global.domains[domain]->slab_domain->caches[SLAB_TYPE_NONPAGEABLE_ZERO];
    struct slab_cache *c = &cs->caches[index];
    void *ret =
        slab_alloc(c, /*handle=*/NULL,
                   ALLOC_BEHAVIOR_NORMAL | SLAB_ALLOC_BEHAVIOR_FROM_ALLOC);

#ifdef DEBUG_ASAN
    if (ret)
        asan_alloc(ret, size, ksize(ret));
#endif

    return ret;
}

/* okay, our free policy (in terms of freequeue usage) is:
 *
 * If the freequeue is the local freequeue, we immediately drain
 * to the slab cache if the freequeue ringbuffer fills up.
 *
 * If the freequeue is a remote freequeue, we first try and add
 * to its ringbuffer. If this fails, then, if the allocation
 * is a slab allocation (did not come from kmalloc_pages), we
 * will add to its freequeue chain if the other allocator is busy
 * (we look at our stats and see that it is doing a LOT of work)
 *
 * Otherwise, if the allocator is not too busy, we just free
 * to the slab cache on the remote side.
 *
 * This freequeue policy is only relevant if we actually
 * choose to use the freequeue. Hopefully most frees can just
 * go to the magazine instead of the freequeue.
 *
 * Later on we'll do GC and all that fun stuff.
 *
 */

bool slab_domain_busy(struct slab_domain *domain) {
    bool idle = domain_idle(domain->domain);
    struct slab_domain_bucket *curr = &domain->buckets[domain->stats->current];
    bool recent_call = curr->alloc_calls && curr->free_calls;

    return recent_call && !idle;
}

bool kfree_free_queue_enqueue(struct slab_domain *domain, void *ptr) {
    struct slab_domain *local = slab_domain_local();
    vaddr_t vptr = (vaddr_t) ptr;

    /* Splendid, it worked */
    if (slab_free_queue_ringbuffer_enqueue(&domain->free_queue, vptr)) {
        slab_stat_free_to_ring(local);
        return true;
    }

    return false;
}

void kfree_pages(void *ptr, enum alloc_behavior behavior) {
    struct slab_page_hdr *header = slab_page_hdr_for_addr(ptr);

    /* early allocations do not have topology data and set their
     * `header->domain` to NULL. in this case, we just assume that
     * we should flush to domain 0 since that is most likely where
     * the allocation had come from. */
    struct slab_domain *owner = header->domain;

    owner = owner ? owner : global.domains[0]->slab_domain;

    /* these pages don't turn into slabs and thus don't get added
     * into the slab caches. instead, we just directly free it to
     * the physical memory allocator. we will first try and append
     * to the freequeue ringbuf.
     *
     * if that fails, if the allocator is busy, we will append it
     * to the freequeue freelist, otherwise just flush the allocation.
     *
     * only touch the freelist if we are looking at a remote domain */

    /* no touchy */
    if (!alloc_behavior_may_fault(behavior))
        kassert(!header->pageable);

    if (kfree_free_queue_enqueue(owner, ptr))
        return;

    /* could not put it on the freequeue... */

    /* TODO: We can try and figure out how to turn these pages
     * back into slabs and recycle them as such... for now, it
     * is fine to just free them to page_alloc */
    slab_free_page_hdr(header, behavior);
}

static bool kfree_try_free_to_magazine(struct slab_percpu_cache *pcpu,
                                       void *ptr, size_t size) {
    struct slab *slab = slab_for_ptr(ptr);

    /* wrong domain */
    if (slab->parent_cache->parent_domain != pcpu->domain)
        return false;

    kassert(slab->type != SLAB_TYPE_NONE);
    if (slab_is_pageable(slab))
        return false;

    int32_t idx = slab_size_to_index(size);
    enum slab_magazine_type mtype =
        slab_is_zeroed(slab) ? SLAB_MAGAZINE_ZERO : SLAB_MAGAZINE_NORMAL;

    if (mtype == SLAB_MAGAZINE_ZERO)
        memset(ptr, 0, ksize(ptr));

    struct slab_magazine *mag = &pcpu->mags[mtype][idx];
    bool ret = slab_magazine_push(mag, (vaddr_t) ptr);
    if (ret)
        slab_stat_free_to_percpu(pcpu->domain);

    return ret;
}

void slab_free(struct slab_domain *domain, void *obj) {
    struct slab *slab = slab_for_ptr(obj);
    struct slab_cache *cache = slab->parent_cache;
    if (slab_is_zeroed(slab))
        memset(obj, 0, cache->obj_size);

    enum irql slab_cache_irql = spin_lock(&cache->lock);
    slab_bitmap_free(slab, obj);

    if (slab->used == 0) {
        slab_move(cache, slab, SLAB_FREE);
        if (slab_should_enqueue_gc(slab)) {
            slab_list_del(slab);
            slab_stat_gc_collection(domain);
            slab_gc_enqueue(domain, slab);
            spin_unlock(&cache->lock, slab_cache_irql);
            return;
        }
    } else if (slab->state == SLAB_FULL) {
        slab_move(cache, slab, SLAB_PARTIAL);
    }

    slab_check_assert(slab);
    spin_unlock(&cache->lock, slab_cache_irql);
}

static size_t slab_free_queue_drain_on_free(struct slab_domain *domain,
                                            struct slab_percpu_cache *pcpu,
                                            enum alloc_behavior behavior) {
    if (!alloc_behavior_may_fault(behavior))
        return 0;

    return slab_free_queue_drain_limited(pcpu, domain, /* pct = */ 100,
                                         behavior);
}

void kfree_new(void *ptr, enum alloc_behavior behavior) {
    enum irql outer = irql_raise(IRQL_DISPATCH_LEVEL);
    kmalloc_ptr_in_slab_validate(ptr);

    size_t size = ksize(ptr);
    int32_t idx = slab_size_to_index(size);
    struct slab_domain *local_domain = slab_domain_local();
    struct slab_percpu_cache *pcpu = slab_percpu_cache_local();

#ifdef DEBUG_SLAB_DEEP
    slab_debug_assert_not_already_free((vaddr_t) ptr, idx);
#endif

    slab_stat_free_call(local_domain);

    /* Free from ASAN first so we don't get a
     * window in between this and slab_free */
    asan_free(ptr, size);

    if (idx < 0) {
        kfree_pages(ptr, behavior);
        goto garbage_collect;
    }

    /* nice, we freed it to the magazine and we are all good now -- fastpath,
     * so we don't try GC or any funny business */
    if (kfree_try_free_to_magazine(pcpu, ptr, size))
        goto done;

    /* did not free to magazine - this is an alloc from a slab */
    struct slab *slab = slab_for_ptr(ptr);
    struct slab_domain *owner = slab->parent_cache->parent_domain;

    if (kfree_free_queue_enqueue(owner, ptr))
        goto done;

    /* could not put on percpu cache or freequeue, now we free to
     * the slab cache that owns this data */

    if (owner == local_domain) {
        slab_stat_free_to_local_slab(local_domain);
    } else {
        slab_stat_free_to_remote_domain(local_domain);
    }

    slab_free(owner, ptr);

garbage_collect:

    slab_free_queue_drain_on_free(local_domain, pcpu, behavior);

done:
    irql_lower(outer);
}

void *kmalloc_init(size_t size, enum alloc_flags f, enum alloc_behavior b) {
    (void) b;
    return kmalloc_old(size, f);
}

void kfree_init(void *p, enum alloc_behavior b) {
    (void) b;
    kfree_old(p);
}

STATIC_CALL_DECLARE(alloc, kmalloc_init);
STATIC_CALL_DECLARE(free, kfree_init);

void slab_switch_to_domain_allocations(void) {
    static_call_update(alloc, kmalloc_new);
    static_call_update(free, kfree_new);
}

void *kmalloc_internal(size_t size, enum alloc_flags flags,
                       enum alloc_behavior behavior) {
    void *p = static_call(alloc)(size, flags, behavior);

#ifdef DEBUG_ASAN
    if (p)
        asan_alloc(p, size, ksize(p));
#endif

#ifdef DEBUG_SLAB
    /* TODO: This should go away on release builds. When we bring
     * in non-fatal assertions/debug only assertions, this should
     * get changed so as to not make the code be slower than a memset 0 */
    if (p && (flags & ALLOC_FLAG_ZERO_ON_ALLOC))
        kassert(is_buffer_uniform(p, size, 0));
#endif

#ifdef DEBUG_SLAB_DEEP
    /* Track the REAL caller (this function is the public entry point). */
    if (p)
        slab_track_event((vaddr_t) p, (uint64_t) __builtin_return_address(0), 0,
                         true);
#endif

    return p;
}

void kfree_internal(void *p, enum alloc_behavior behavior) {
    if (unlikely(!p))
        return;

    if ((uint16_t) behavior == (uint16_t) ALLOC_FLAGS_DEFAULT) {
        slab_warn("Likely incorrect arguments passed into `kfree`");
        return;
    }

#ifdef DEBUG_SLAB_DEEP
    slab_track_event((vaddr_t) p, (uint64_t) __builtin_return_address(0), 0,
                     false);
#endif

#ifdef DEBUG_SLAB
    memset(p, 0x67, ksize(p));
#endif

#ifdef DEBUG_ASAN
    asan_free(p, ksize(p));
#endif

    static_call(free)(p, behavior);
}

void *krealloc_internal(void *ptr, size_t size, enum alloc_flags flags,
                        enum alloc_behavior behavior) {
    if (!ptr)
        return kmalloc(size, flags, behavior);

    if (size == 0) {
        kfree(ptr, behavior);
        return NULL;
    }

    size_t old = ksize(ptr);

    /* Touch nothing. This can still use the same slab allocation */
    size_t old_idx = slab_size_to_index(old);
    size_t new_idx = slab_size_to_index(size);
    if (old_idx == new_idx) {
#ifdef DEBUG_ASAN
        asan_alloc(ptr, size, old);
#endif
        return ptr;
    }

    void *new_ptr = kmalloc(size, flags, behavior);

    if (!new_ptr)
        return NULL;

    size_t to_copy = (old < size) ? old : size;
    memcpy(new_ptr, ptr, to_copy);
    kfree(ptr);
    return new_ptr;
}

#ifdef TEST_MEM
TEST_EXPORT(slab_map_new);
#endif
