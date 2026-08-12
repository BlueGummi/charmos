#include <math/bit_ops.h>
#include <math/div.h>
#include <math/ilog2.h>
#include <math/to_bits_bytes.h>
#include <mem/asan.h>
#include <mem/hhdm.h>
#include <mem/pmm.h>
#include <mem/vas.h>

#include "internal.h"

static inline vaddr_t vaddr_to_base_addr(vaddr_t vaddr) {
    return vaddr >> PAGE_4K_SHIFT;
}

static inline vaddr_t base_addr_to_vaddr(vaddr_t base_addr) {
    return base_addr << PAGE_4K_SHIFT;
}

static void validate_ptr_in_chunk(struct slab_chunk *chunk, void *ptr) {
    vaddr_t vaddr = (vaddr_t) ptr;
    vaddr_t min = base_addr_to_vaddr(chunk->base_addr);
    vaddr_t max = base_addr_to_vaddr(chunk->base_addr) + SLAB_CHUNK_SIZE;
    kassert(vaddr >= min && vaddr <= max);
}

static struct list_head *chunk_list_for(struct slab_chunks *sc,
                                        enum slab_chunk_state s) {
    switch (s) {
    case SLAB_CHUNK_PARTIAL: return &sc->partial_list;
    case SLAB_CHUNK_USED: return &sc->used_list;
    default: kassert_unreachable("invalid slab chunk state");
    }
}

static void destroy_chunk(struct slab_chunks *sc, struct slab_chunk *c) {
    vaddr_t vaddr = base_addr_to_vaddr(c->base_addr);
    uint8_t curr = slab_order_map_get(vaddr);
    kassert(curr != SLAB_POW2_ORDER_EMPTY);

#ifdef DEBUG_ASAN
    asan_free((void *) vaddr, SLAB_CHUNK_SIZE);
    asan_shadow_release(vaddr, SLAB_CHUNK_SIZE);
#endif

    vas_free(slab_global.vas, vaddr, PAGE_2MB);
    slab_order_map_set(vaddr, SLAB_POW2_ORDER_EMPTY);

    fixed_size_free(&sc->fsr, c);
}

static bool move_to(struct slab_chunks *sc, struct slab_chunk *c,
                    enum slab_chunk_state s) {
    kassert(c->state != s);
    list_del_init(&c->list);
    if (s == SLAB_CHUNK_FREE) {
        destroy_chunk(sc, c);
        return true;
    }
    c->state = s;
    list_add_tail(&c->list, chunk_list_for(sc, s));
    return false;
}

static struct slab_chunk *alloc_chunk(struct slab_chunks *sc,
                                      enum irql *lirql) {
    vaddr_t base = vas_alloc(slab_global.vas, PAGE_2MB, PAGE_2MB);

    if (!base)
        return NULL;

    uint8_t curr = slab_order_map_get(base);
    kassert(curr == SLAB_POW2_ORDER_EMPTY);
    slab_order_map_set(base, sc->pow2_order);

    spin_unlock(&sc->lock, *lirql);
    struct slab_chunk *ret = fixed_size_alloc(&sc->fsr);

#ifdef DEBUG_ASAN
    /* We have to do this here as the shadow must exist before alloc */
    if (ret && asan_shadow_install(base, SLAB_CHUNK_SIZE) < 0) {
        fixed_size_free(&sc->fsr, ret);
        ret = NULL;
    }
#endif

    *lirql = spin_lock(&sc->lock);

    if (!ret) {
        slab_order_map_set(base, SLAB_POW2_ORDER_EMPTY);
        vas_free(slab_global.vas, base, PAGE_2MB);
        return NULL;
    }

    INIT_LIST_HEAD(&ret->list);
    ret->owner = sc;
    ret->state = SLAB_CHUNK_PARTIAL;
    ret->base_addr = vaddr_to_base_addr(base);
    ret->used = 0;
    memset(ret->bitmap, 0, sc->bitmap_bytes);
    list_add_tail(&ret->list, &sc->partial_list);
    return ret;
}

static vaddr_t alloc_from(struct slab_chunks *chunks,
                          struct slab_chunk *chunk) {
    uint64_t *bm = (uint64_t *) chunk->bitmap;
    size_t nwords = DIV_ROUND_UP(chunks->bitmap_bytes * 8, 64);

    for (size_t w = 0; w < nwords; w++) {
        if (bm[w] != UINT64_MAX) {
            uint64_t free_bits = ~bm[w];
            uint64_t bit = __builtin_ctzll(free_bits);
            uint64_t i = w * 64 + bit;

            if (i >= chunks->bitmap_bytes * 8)
                break;

            SLAB_BITMAP_SET(bm[w], 1ULL << bit);
            chunk->used++;

            return base_addr_to_vaddr(chunk->base_addr) +
                   i * chunks->page_stride * PAGE_SIZE;
        }
    }

    kassert_unreachable("alloc_from slab chunk should not fail");
}

static void free_to(struct slab_chunks *chunks, struct slab_chunk *chunk,
                    vaddr_t v) {
    validate_ptr_in_chunk(chunk, (void *) v);
    uint64_t offset = v - base_addr_to_vaddr(chunk->base_addr);
    kassert(offset % chunks->page_stride == 0);
    uint64_t i = offset / chunks->page_stride;

    uint64_t *bm = (uint64_t *) chunk->bitmap;
    size_t nwords = DIV_ROUND_UP(chunks->bitmap_bytes * 8, 64);
    kassert(i < nwords * 64);

    size_t w = i / 64;
    size_t b = i % 64;
    kassert(SLAB_BITMAP_TEST(bm[w], 1ULL << b));
    SLAB_BITMAP_UNSET(bm[w], 1ULL << b);
    chunk->used--;
}

static vaddr_t chunk_alloc(struct slab_chunks *chunks,
                           struct slab_chunk *chunk) {
    size_t max = chunks->bitmap_bytes * 8;
    vaddr_t ret = alloc_from(chunks, chunk);
    if (chunk->used == max)
        move_to(chunks, chunk, SLAB_CHUNK_USED);

    return ret;
}

static bool chunk_free(struct slab_chunks *chunks, struct slab_chunk *chunk,
                       vaddr_t v) {
    enum slab_chunk_state last = chunk->state;
    kassert(last != SLAB_CHUNK_FREE);
    free_to(chunks, chunk, v);

    if (last == SLAB_CHUNK_USED) {
        move_to(chunks, chunk, SLAB_CHUNK_PARTIAL);
    } else if (chunk->used == 0) {
        return move_to(chunks, chunk, SLAB_CHUNK_FREE);
    }

    return false;
}

static struct slab_chunk *try_pop(struct list_head *lh) {
    struct list_head *pop = list_pop_front_init(lh);
    if (!pop)
        return NULL;

    return container_of(pop, struct slab_chunk, list);
}

vaddr_t slab_chunks_alloc(struct slab_chunks *sc, struct slab_chunk **out) {
    vaddr_t ret = 0x0;
    enum irql irql = spin_lock(&sc->lock);

    *out = try_pop(&sc->partial_list);
    if (!*out) {
        if (!(*out = alloc_chunk(sc, &irql)))
            goto out;
    }

    ret = chunk_alloc(sc, *out);

out:
    spin_unlock(&sc->lock, irql);
    return ret;
}

void slab_chunks_free(struct slab_chunks *sc, struct slab_chunk *chunk,
                      vaddr_t addr) {
    enum irql irql = spin_lock(&sc->lock);

    chunk_free(sc, chunk, addr);

    spin_unlock(&sc->lock, irql);
}

void slab_chunks_init(struct slab_chunks *sc, struct slab_cache *parent) {
    sc->parent = parent;
    sc->page_stride = next_pow2(parent->pages_per_slab);
    size_t page_count = 1 << (PAGE_2M_SHIFT - PAGE_4K_SHIFT);
    size_t bitmap_bits = DIV_ROUND_UP(page_count, sc->page_stride);
    sc->bitmap_bytes = to_bytes(bitmap_bits);
    INIT_LIST_HEAD(&sc->partial_list);
    INIT_LIST_HEAD(&sc->used_list);

    spinlock_init(&sc->lock);
    sc->pow2_order = ilog2(sc->page_stride);

    struct fixed_size_range_attributes attrs = {
        .obj_size = sizeof(struct slab_chunk) + sc->bitmap_bytes,
        .obj_align = _Alignof(struct slab_chunk),
        .init_obj = NULL,
        .deinit_obj = NULL,
        .bootstrap_mode = false,
    };
    fixed_size_range_init(&sc->fsr, &attrs);
}
