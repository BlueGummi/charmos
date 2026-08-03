#include <mem/alloc_or_die.h>
#include <mem/slab.h>
#include <sch/sched.h>
#include <smp/domain.h>

#include "internal.h"

static void slab_magazine_zero_check(struct slab_magazine *mag) {
#ifdef DEBUG_SLAB_DEEP
    if (mag->type != SLAB_MAGAZINE_ZERO)
        return;

    for (size_t i = 0; i < SLAB_MAG_ENTRIES; i++) {
        if (mag->objs[i]) {
            if (!is_buffer_uniform((void *) mag->objs[i], mag->obj_size, 0)) {
                vaddr_t obj = mag->objs[i];
                struct slab *s = slab_for_ptr((void *) obj);
                uint64_t byte_idx;
                uint8_t bit_mask;
                slab_index_and_mask(s, (void *) obj, &byte_idx, &bit_mask);
                slab_err("CORRUPT parked obj %p mag_objsize=%zu", obj,
                         mag->obj_size);
                slab_err("  slab=%p type=%d obj_size=%zu page_count=%zu "
                         "used=%zu bit_set=%d",
                         s, s->type, s->parent_cache->obj_size, s->page_count,
                         s->used,
                         SLAB_BITMAP_TEST(s->bitmap[byte_idx], bit_mask) != 0);
                for (size_t b = 0; b < mag->obj_size; b++) {
                    uint8_t v = ((uint8_t *) obj)[b];
                    if (v)
                        slab_err("  byte %zu = 0x%02x", b, v);
                }
                panic("not uniform at idx %zu on mag %p with count %zu\n", i,
                      mag, mag->count);
            }
        }
    }
#endif
}

static bool slab_magazine_has(struct slab_magazine *mag, vaddr_t obj) {
    for (size_t i = 0; i < SLAB_MAG_ENTRIES; i++) {
        if (mag->objs[i] == obj)
            return true;
    }
    return false;
}

bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj) {
#ifdef DEBUG_SLAB
    if (mag->type == SLAB_MAGAZINE_ZERO &&
        !is_buffer_uniform((void *) obj, mag->obj_size, 0))
        panic("buffer of size %zu is not uniform\n", mag->obj_size);
#endif

    kassert(mag->parent == slab_percpu_cache_local());
    kassert(!slab_magazine_has(mag, obj));
    slab_magazine_zero_check(mag);

    if (mag->count < SLAB_MAG_ENTRIES) {
        mag->objs[mag->count++] = obj;
        return true;
    }

    return false;
}

vaddr_t slab_magazine_pop(struct slab_magazine *mag) {
    kassert(mag->parent == slab_percpu_cache_local());

    vaddr_t ret = 0x0;

    if (mag->count > 0) {
        ret = mag->objs[--mag->count];
        mag->objs[mag->count] = 0x0; /* Reset it */
    }

    if (ret)
        kassert(!slab_magazine_has(mag, ret));

    slab_magazine_zero_check(mag);
    return ret;
}

static size_t slab_cache_bulk_alloc(struct slab_cache *cache,
                                    vaddr_t *addr_array, size_t num_objects,
                                    enum alloc_behavior behavior) {
    size_t total_allocated = 0;

    for (size_t i = 0; i < num_objects; i++) {

        /* We don't allow allocations of new slabs - that
         * is not the point of our percpu caches */
        kassert(!(behavior & SLAB_ALLOC_BEHAVIOR_FROM_ALLOC));
        void *obj = slab_alloc(cache, /*handle=*/NULL, behavior);
        if (!obj)
            break;

        total_allocated++;
        addr_array[i] = (vaddr_t) obj;
    }

    return total_allocated;
}

static void slab_cache_bulk_free(struct slab_domain *domain,
                                 vaddr_t *addr_array, size_t num_objects) {
    for (size_t i = 0; i < num_objects; i++) {
        vaddr_t addr = addr_array[i];

        if (!slab_free_queue_ringbuffer_enqueue(&domain->free_queue, addr))
            slab_free(domain, (void *) addr);
    }

    return;
}

/* NOTE: unused? */
void slab_percpu_flush(struct slab_domain *dom, struct slab_percpu_cache *pc,
                       size_t class_idx, vaddr_t overflow_obj) {
    for (int i = 0; i < SLAB_MAGAZINE_TYPE_COUNT; i++) {
        struct slab_magazine *mag = &pc->mags[i][class_idx];

        /* capture how many valid items we have */
        size_t valid = mag->count;

        /* copy only valid objects */
        for (size_t i = 0; i < valid; i++)
            pc->shadow_objs[i] = mag->objs[i];

        /* reset magazine */
        mag->count = 0;
        for (size_t i = 0; i < valid; i++)
            mag->objs[i] = 0x0;

        /* add overflow object to the end and free (valid + 1) objects */
        pc->shadow_objs[valid] = overflow_obj;
        slab_cache_bulk_free(dom, pc->shadow_objs, valid + 1);
    }
}

static vaddr_t slab_percpu_refill_for_mag_and_cache(
    struct slab_percpu_cache *pc, struct slab_magazine *mag,
    struct slab_cache *cache, enum alloc_behavior behavior) {
    size_t space = SLAB_MAG_ENTRIES - mag->count;

    if (space == 0)
        space = 1; /* we still want 1 so we can return an object */

    kassert(space <= SLAB_MAG_ENTRIES);

    if (mag->type == SLAB_MAGAZINE_ZERO)
        kassert(cache->type == SLAB_TYPE_PAGEABLE_ZERO ||
                cache->type == SLAB_TYPE_NONPAGEABLE_ZERO);

    size_t want = space;
    size_t got = slab_cache_bulk_alloc(cache, pc->shadow_objs, want, behavior);
    if (got == 0)
        return slab_magazine_pop(mag);

    size_t can_insert = SLAB_MAG_ENTRIES - mag->count;
    size_t to_insert = (got > 0) ? (got - 1) : 0;
    if (to_insert > can_insert)
        to_insert = can_insert;

    kassert(mag->obj_size == cache->obj_size);
    vaddr_t first = pc->shadow_objs[0];

    for (size_t i = 1; i <= to_insert; i++) {

#ifdef DEBUG_SLAB
        if (mag->type == SLAB_MAGAZINE_ZERO &&
            !is_buffer_uniform((void *) pc->shadow_objs[i], mag->obj_size, 0))
            panic("buffer of size %zu is not uniform\n", mag->obj_size);
#endif

        mag->objs[mag->count++] = pc->shadow_objs[i];
        pc->shadow_objs[i] = 0;
    }

    slab_magazine_zero_check(mag);

    return first;
}

static vaddr_t slab_percpu_refill_class(struct slab_domain *dom,
                                        struct slab_percpu_cache *pc,
                                        size_t class_idx,
                                        enum alloc_flags flags,
                                        enum alloc_behavior behavior) {
    struct slab_magazine *mag;
    struct slab_cache *cache;

    if (flags & ALLOC_FLAG_ZERO_ON_ALLOC) {
        mag = &pc->mags[SLAB_MAGAZINE_ZERO][class_idx];
        cache = &dom->caches[SLAB_TYPE_NONPAGEABLE_ZERO]->caches[class_idx];
    } else {
        mag = &pc->mags[SLAB_MAGAZINE_NORMAL][class_idx];
        cache = &dom->caches[SLAB_TYPE_NONPAGEABLE]->caches[class_idx];
    }

    return slab_percpu_refill_for_mag_and_cache(pc, mag, cache, behavior);
}

void slab_percpu_refill(struct slab_domain *dom,
                        struct slab_percpu_cache *cache, enum alloc_flags flags,
                        enum alloc_behavior behavior) {
    /* This flushes a portion of the freequeue into the percpu cache */
    slab_free_queue_drain_limited(cache, dom, /* pct = */ 100, behavior);
    for (size_t class = 0; class < slab_global.num_sizes; class++)
        slab_percpu_refill_class(dom, cache, class, flags, behavior);
}

/* TODO: memory locality */
void slab_domain_percpu_init(struct slab_domain *domain) {
    size_t cpus = domain->domain->num_cores;
    domain->percpu_caches = alloc_or_die(
        kmalloc(sizeof(struct slab_percpu_cache *) * cpus, ALLOC_FLAGS_ZERO));

    for (size_t i = 0; i < cpus; i++) {
        domain->percpu_caches[i] = alloc_or_die(
            kmalloc(sizeof(struct slab_percpu_cache), ALLOC_FLAGS_ZERO));
        for (int j = 0; j < SLAB_MAGAZINE_TYPE_COUNT; j++) {
            domain->percpu_caches[i]->mags[j] = alloc_or_die(
                kmalloc(sizeof(struct slab_magazine) * slab_global.num_sizes,
                        ALLOC_FLAGS_ZERO));
            for (size_t k = 0; k < slab_global.num_sizes; k++) {
                domain->percpu_caches[i]->mags[j][k].type = j;
                domain->percpu_caches[i]->mags[j][k].obj_size =
                    slab_global.class_sizes[k].size;
                domain->percpu_caches[i]->mags[j][k].parent =
                    domain->percpu_caches[i];
            }
        }

        domain->percpu_caches[i]->domain = domain;
        mpsc_slist_init(&domain->percpu_caches[i]->defer_frees);
    }
}
