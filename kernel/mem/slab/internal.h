#pragma once
#include <mem/page.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>

#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL
#define SLAB_OBJ_ALIGN 16u
#define SLAB_MIN_SHIFT 4
#define SLAB_MAX_SHIFT 10
#define SLAB_CLASS_COUNT (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)
#define SLAB_BITMAP_TEST(__bitmap, __idx) (__bitmap & __idx)

enum slab_state { SLAB_FREE, SLAB_PARTIAL, SLAB_FULL };

struct slab {
    struct slab *next;
    struct slab *prev;

    uint8_t *bitmap;
    void *mem;
    struct page *backing_page;
    atomic_uint_fast64_t used;
    enum slab_state state;
    struct slab_cache *parent_cache;
};
#define PAGE_NON_SLAB_SPACE (PAGE_SIZE - sizeof(struct slab))

struct slab_cache {
    uint64_t obj_size;
    uint64_t objs_per_slab;
    uint64_t max_objects; 

    struct slab *slabs_free;
    struct slab *slabs_partial;
    struct slab *slabs_full;
    uint64_t free_slabs_count;
};

struct slab_phdr {
    uint32_t magic;
    uint64_t pages;
};

void slab_init();
static inline void slab_list_remove(struct slab **list, struct slab *slab) {
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        *list = slab->next;

    if (slab->next)
        slab->next->prev = slab->prev;

    slab->next = slab->prev = NULL;
}

static inline void slab_list_add(struct slab **list, struct slab *slab) {
    slab->next = *list;

    if (*list)
        (*list)->prev = slab;

    slab->prev = NULL;
    *list = slab;
}

static inline uint64_t slab_round_up_pow2(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static inline struct slab *slab_for_ptr(void *ptr) {
    void *raw_obj = (uint8_t *) ptr - sizeof(struct slab *);
    struct slab *slab = *((struct slab **) raw_obj);
    return slab;
}

extern struct vas_space *slab_vas;
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
