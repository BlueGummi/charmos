#include "internal.h"
#include <mem/address_range.h>

#ifdef DEBUG_SLAB_DEEP
const char *slab_addr_region(vaddr_t a) {
    if (address_range_for_addr(a))
        return address_range_for_addr(a)->name;

    return "none";
}

#define SLAB_TRACK_SLOTS 16384
struct slab_track {
    vaddr_t addr;
    uint64_t alloc_ra[2];
    uint64_t free_ra[2];
    uint32_t seq;
    uint8_t live;
};
static struct slab_track slab_track_table[SLAB_TRACK_SLOTS];
static _Atomic uint32_t slab_track_seq;

void slab_track_event(vaddr_t addr, uint64_t ra0, uint64_t ra1, bool is_alloc) {
    struct slab_track *e = &slab_track_table[(addr >> 6) % SLAB_TRACK_SLOTS];
    e->addr = addr;
    e->seq =
        atomic_fetch_add_explicit(&slab_track_seq, 1, memory_order_relaxed);
    if (is_alloc) {
        e->alloc_ra[0] = ra0;
        e->alloc_ra[1] = ra1;
        e->live = 1;
    } else {
        e->free_ra[0] = ra0;
        e->free_ra[1] = ra1;
        e->live = 0;
    }
}

void slab_track_dump(const char *label, vaddr_t addr) {
    struct slab_track *e = &slab_track_table[(addr >> 6) % SLAB_TRACK_SLOTS];
    if (e->addr != addr) {
        slab_err("  track[%s %p]: no record (last-seen addr %p)", label,
                 (void *) addr, (void *) e->addr);
        return;
    }
    slab_err("  track[%s %p]: state=%s seq=%u alloc_ra=%#lx/%#lx "
             "free_ra=%#lx/%#lx",
             label, (void *) addr, e->live ? "LIVE(alloc)" : "freed", e->seq,
             e->alloc_ra[0], e->alloc_ra[1], e->free_ra[0], e->free_ra[1]);
}

void slab_debug_assert_not_already_free(vaddr_t v, int32_t class) {
    if (class < 0)
        return;
    for (size_t d = 0; d < global.domain_count; d++) {
        struct slab_domain *sd = global.domains[d]->slab_domain;
        if (!sd)
            continue;

        if (sd->percpu_caches) {
            size_t cpus = sd->domain->num_cores;
            for (size_t c = 0; c < cpus; c++) {
                struct slab_percpu_cache *pc = sd->percpu_caches[c];
                if (!pc)
                    continue;
                for (int t = 0; t < SLAB_MAGAZINE_TYPE_COUNT; t++) {
                    if (!pc->mags[t])
                        continue;
                    struct slab_magazine *m = &pc->mags[t][class];
                    for (size_t i = 0; i < m->count; i++)
                        if (m->objs[i] == v)
                            panic("DOUBLE FREE: %p already parked in d=%zu "
                                  "cpu=%zu mag type=%d class=%d idx=%zu",
                                  (void *) v, d, c, t, class, i);
                }
            }
        }

        struct slab_free_queue *fq = &sd->free_queue;
        if (fq->mpmc.slots) {
            uint64_t tail =
                atomic_load_explicit(&fq->mpmc.tail, memory_order_acquire);
            uint64_t head =
                atomic_load_explicit(&fq->mpmc.head, memory_order_acquire);
            for (uint64_t pos = tail; pos != head; pos++)
                if (fq->mpmc.slots[pos % fq->mpmc.capacity].data == v)
                    panic(
                        "DOUBLE FREE: %p already in d=%zu free_queue slot=%zu",
                        (void *) v, d, (size_t) (pos % fq->mpmc.capacity));
        }
    }
}

__no_sanitize_address void
slab_dump_corruption(void *obj, struct slab_magazine *popped_mag,
                     size_t obj_size) {
    panic_broadcast_nmi(); /* Get everyone to stop yapping
                            * so we have a clean view */
    vaddr_t v = (vaddr_t) obj;
    struct slab *s = slab_for_ptr(obj);
    uint64_t byte_idx;
    uint8_t bit_mask;
    slab_index_and_mask(s, obj, &byte_idx, &bit_mask);
    size_t obj_idx = (v - s->mem) / s->parent_cache->obj_stride;

    slab_err("obj=%p region=%s", (void *) v, slab_addr_region(v));
    slab_err("slab=%p base_off=%#zx obj_idx=%zu type=%d obj_size=%zu "
             "page_count=%zu used=%zu bit_set=%d",
             s, (size_t) (v - (vaddr_t) s), obj_idx, s->type,
             s->parent_cache->obj_size, s->page_count, s->used,
             SLAB_BITMAP_TEST(s->bitmap[byte_idx], bit_mask) != 0);

    slab_track_dump("victim", v);

    size_t words;

    if (popped_mag) {
        words = popped_mag->obj_size / sizeof(uint64_t);
    } else {
        words = obj_size / sizeof(uint64_t);
    }

    for (size_t w = 0; w < words; w++) {
        uint64_t val = ((uint64_t *) obj)[w];
        if (!val)
            continue;
        slab_err("  word[%zu] (off %zu) = %#lx region=%s", w, w * 8, val,
                 slab_addr_region((vaddr_t) val));
        if ((vaddr_t) val >= SLAB_HEAP_START && (vaddr_t) val < SLAB_HEAP_END) {
            struct slab *ps = slab_for_ptr((void *) val);
            slab_err("    -> points into slab %p type=%d obj_size=%zu", ps,
                     ps->type, ps->parent_cache->obj_size);
            slab_track_dump("target", (vaddr_t) val);
        }
        /* Dump 8 words around the target if it looks like a kernel pointer. */
        if ((vaddr_t) val >= SLAB_HEAP_START &&
            vmm_get_phys(PAGE_ALIGN_DOWN((vaddr_t) val), VMM_FLAG_NONE) !=
                (paddr_t) -1) {
            uint64_t *t = (uint64_t *) (vaddr_t) val;
            for (int j = 0; j < 6; j++)
                slab_err("      [%p +%d] = %#lx", t, j * 8, t[j]);
        }
    }

    /* Search every magazine, shadow buffer, and free_queue for this address. */
    size_t found = 0;
    for (size_t d = 0; d < global.domain_count; d++) {
        struct slab_domain *sd = global.domains[d]->slab_domain;
        if (!sd)
            continue;

        if (sd->percpu_caches) {
            size_t cpus = sd->domain->num_cores;
            for (size_t c = 0; c < cpus; c++) {
                struct slab_percpu_cache *pc = sd->percpu_caches[c];
                if (!pc)
                    continue;

                vaddr_t pcv = (vaddr_t) pc;
                if (v >= pcv && v < pcv + sizeof(struct slab_percpu_cache))
                    slab_err(
                        "  !! obj OVERLAPS percpu_cache %p (d=%zu cpu=%zu) "
                        "off_into_pc=%#zx",
                        pc, d, c, (size_t) (v - pcv));
                for (int t = 0; t < SLAB_MAGAZINE_TYPE_COUNT; t++) {
                    if ((vaddr_t) pc->mags[t] ==
                        (vaddr_t) ((uint64_t *) obj)[6])
                        slab_err("  !! obj word[6] == percpu_cache %p mags[%d] "
                                 "(d=%zu cpu=%zu)",
                                 pc, t, d, c);
                }
                for (int t = 0; t < SLAB_MAGAZINE_TYPE_COUNT; t++) {
                    if (!pc->mags[t])
                        continue;
                    for (size_t k = 0; k < slab_global.num_sizes; k++) {
                        struct slab_magazine *m = &pc->mags[t][k];
                        for (size_t i = 0; i < SLAB_MAG_ENTRIES; i++) {
                            if (m->objs[i] == v) {
                                slab_err("  ALSO IN mag d=%zu cpu=%zu type=%d "
                                         "class=%zu idx=%zu",
                                         d, c, t, k, i);
                                found++;
                            }
                        }
                    }
                    for (size_t i = 0; i < SLAB_MAG_ENTRIES + 1; i++) {
                        if (pc->shadow_objs[i] == v) {
                            slab_err("  ALSO IN shadow d=%zu cpu=%zu idx=%zu",
                                     d, c, i);
                            found++;
                        }
                    }
                }
            }
        }

        struct slab_free_queue *fq = &sd->free_queue;
        if (fq->mpmc.slots) {
            uint64_t tail =
                atomic_load_explicit(&fq->mpmc.tail, memory_order_acquire);
            uint64_t head =
                atomic_load_explicit(&fq->mpmc.head, memory_order_acquire);
            for (size_t i = 0; i < fq->mpmc.capacity; i++) {
                if (fq->mpmc.slots[i].data != v)
                    continue;
                bool occupied = false;
                for (uint64_t pos = tail; pos != head; pos++) {
                    if (pos % fq->mpmc.capacity == i) {
                        occupied = true;
                        break;
                    }
                }
                slab_err("  ALSO IN free_queue d=%zu slot=%zu %s", d, i,
                         occupied ? "(OCCUPIED -- real dup)" : "(stale addr)");
                if (occupied)
                    found++;
            }
        }
    }
    slab_err("total duplicate locations found (excl. nothing): %zu", found);
}

stack_handle_t slab_stack_handle_for(void *ptr) {
    kassert(slab_ptr_in_slab(ptr));
    struct slab *s = slab_for_ptr(ptr);
    size_t idx = slab_allocation_index(s, ptr);
    return s->traces[idx];
}
#endif
