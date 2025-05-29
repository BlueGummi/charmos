#include <pmm.h>
#include <printf.h>
#include <slab.h>
#include <string.h>
#include <vmm.h>

struct slab_cache slab_caches[SLAB_CLASS_COUNT];
uintptr_t slab_heap_top = 0xFFFF800000000000;

static void *slab_map_new_page() {
    uintptr_t phys = (uintptr_t) pmm_alloc_pages(1, false);
    if (!phys) {
        return NULL;
    }

    uintptr_t virt = slab_heap_top;
    slab_heap_top += PAGE_SIZE;

    vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE);
    return (void *) virt;
}

static void slab_cache_init(struct slab_cache *cache, size_t obj_size) {
    cache->obj_size = obj_size + sizeof(struct slab *);
    size_t bitmap_size =
        (PAGE_SIZE - sizeof(struct slab)) / (cache->obj_size + 1 / 8);
    cache->objs_per_slab = bitmap_size;
    cache->slabs_free = NULL;
    cache->slabs_partial = NULL;
    cache->slabs_full = NULL;
}

static struct slab *slab_create(struct slab_cache *cache) {
    void *page = slab_map_new_page();
    if (!page) {
        return NULL;
    }

    struct slab *slab = (struct slab *) page;
    slab->parent_cache = cache;

    size_t bitmap_bytes = (cache->objs_per_slab + 7) / 8;
    slab->bitmap = (uint8_t *) ((uint8_t *) page + sizeof(struct slab));

    uintptr_t mem_ptr = (uintptr_t) (slab->bitmap + bitmap_bytes);
    mem_ptr = (mem_ptr + cache->obj_size - 1) & ~(cache->obj_size - 1);
    slab->mem = (void *) mem_ptr;

    slab->used = 0;
    slab->state = SLAB_FREE;
    slab->next = NULL;

    memset(slab->bitmap, 0, bitmap_bytes);
    return slab;
}

static void slab_list_remove(struct slab **list, struct slab *slab) {
    while (*list && *list != slab) {
        list = &(*list)->next;
    }
    if (*list == slab) {
        *list = slab->next;
    }
}

static void slab_list_add(struct slab **list, struct slab *slab) {
    slab->next = *list;
    *list = slab;
}

static void *slab_alloc_from(struct slab_cache *cache, struct slab *slab) {
    for (size_t i = 0; i < cache->objs_per_slab; i++) {
        if (!(slab->bitmap[i / 8] & (1 << (i % 8)))) {
            slab->bitmap[i / 8] |= (1 << (i % 8));
            slab->used++;

            if (slab->used == cache->objs_per_slab) {
                slab->state = SLAB_FULL;
                slab_list_remove(&cache->slabs_partial, slab);
                slab_list_add(&cache->slabs_full, slab);
            } else if (slab->used == 1) {
                slab->state = SLAB_PARTIAL;
                slab_list_remove(&cache->slabs_free, slab);
                slab_list_add(&cache->slabs_partial, slab);
            }

            uint8_t *obj = (uint8_t *) slab->mem + i * cache->obj_size;
            *((struct slab **) obj) = slab;
            return obj + sizeof(struct slab *);
        }
    }
    return NULL;
}

static void *slab_alloc(struct slab_cache *cache) {
    for (struct slab *slab = cache->slabs_partial; slab; slab = slab->next) {
        void *obj = slab_alloc_from(cache, slab);
        if (obj)
            return obj;
    }

    for (struct slab *slab = cache->slabs_free; slab; slab = slab->next) {
        void *obj = slab_alloc_from(cache, slab);
        if (obj)
            return obj;
    }

    struct slab *slab = slab_create(cache);
    if (!slab)
        return NULL;

    slab_list_add(&cache->slabs_free, slab);
    return slab_alloc_from(cache, slab);
}

static void slab_free(struct slab_cache *cache, struct slab *slab, void *obj) {
    obj = (uint8_t *) obj - sizeof(struct slab *);

    size_t index = ((uintptr_t) obj - (uintptr_t) slab->mem) / cache->obj_size;
    if (!(slab->bitmap[index / 8] & (1 << (index % 8)))) {
        k_panic("Double free or invalid free\n");
    }

    slab->bitmap[index / 8] &= ~(1 << (index % 8));
    slab->used--;

    if (slab->used == 0) {
        slab->state = SLAB_FREE;
        slab_list_remove(&cache->slabs_partial, slab);
        slab_list_add(&cache->slabs_free, slab);
    } else if (slab->state == SLAB_FULL) {
        slab->state = SLAB_PARTIAL;
        slab_list_remove(&cache->slabs_full, slab);
        slab_list_add(&cache->slabs_partial, slab);
    }
}

static int size_to_index(size_t size) {
    if (size == 0)
        return -1;
    int shift = SLAB_MIN_SHIFT;
    while ((1UL << shift) < size + sizeof(struct slab *) &&
           shift <= SLAB_MAX_SHIFT) {
        shift++;
    }
    return (shift > SLAB_MAX_SHIFT) ? -1 : shift - SLAB_MIN_SHIFT;
}

void slab_init() {
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        size_t size = 1UL << (i + SLAB_MIN_SHIFT);
        slab_cache_init(&slab_caches[i], size);
    }
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    int idx = size_to_index(size);
    if (idx < 0)
        return NULL;
    return slab_alloc(&slab_caches[idx]);
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (!ptr)
        return NULL;

    memset(ptr, 0, size);

    return ptr;
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    void *raw_obj = (uint8_t *) ptr - sizeof(struct slab *);
    struct slab *slab = *((struct slab **) raw_obj);
    if (!slab) {
        k_panic("kfree: no slab header found for %p\n", ptr);
    }

    struct slab_cache *cache = slab->parent_cache;
    slab_free(cache, slab, ptr);
}
