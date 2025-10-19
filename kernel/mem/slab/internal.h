#pragma once
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>

#define KMALLOC_PAGE_MAGIC 0xC0FFEE42
#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL
#define SLAB_OBJ_ALIGN 16u
#define SLAB_MIN_SHIFT 4
#define SLAB_MAX_SHIFT 10
#define SLAB_CLASS_COUNT (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)
#define SLAB_BITMAP_TEST(__bitmap, __idx) (__bitmap & __idx)
#define SLAB_CROSSFREE_RING_SIZE 64
#define SLAB_NONPAGEABLE_RESERVED_RATIO (1 / 16)
#define SLAB_DESTROY_HIGH_WATERMARK 4
#define SLAB_INTERLEAVE_STRIDE 1
#define SLAB_PER_CORE_MAGAZINE_ENTRIES 32

enum slab_state { SLAB_FREE, SLAB_PARTIAL, SLAB_FULL };

struct slab {
    struct slab *next, *prev;

    uint8_t *bitmap;
    vaddr_t mem;
    atomic_uint_fast64_t used;
    struct slab_cache *parent_cache;

    enum slab_state state;
};

static inline struct page *slab_get_backing_page(struct slab *slab) {
    return page_for_pfn(PAGE_TO_PFN(vmm_get_phys((vaddr_t) slab)));
}

#define PAGE_NON_SLAB_SPACE (PAGE_SIZE - sizeof(struct slab))

struct slab_magazine {
    void *objs[SLAB_PER_CORE_MAGAZINE_ENTRIES];
    uint8_t head;
    uint8_t count;
};

struct slab_per_cpu_cache {
    struct slab_magazine mag[SLAB_CLASS_COUNT];
    struct slab *active_slab[SLAB_CLASS_COUNT];

    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_per_cpu_cache, lock);

struct slab_free_queue {
    atomic_uint head;
    atomic_uint tail;
    struct spinlock lock;
    size_t capacity;
    vaddr_t *slots;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_free_queue, lock);

struct slab_cache_group_data {
    atomic_size_t total_free_slabs;
};

struct slab_cache {
    struct slab_cache_group_data *cache_group_data;

    uint64_t obj_size;
    uint64_t objs_per_slab;
    uint64_t max_objects; /* Max amount of objects for
                           * a slab in this slab cache */

    struct slab *slabs_free;
    struct slab *slabs_partial;
    struct slab *slabs_full;
    uint64_t free_slabs_count;

    struct list_head list_node;
    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_cache, lock);

struct slab_cache_list {
    struct slab_cache_group_data *group_data;
    struct list_head list;
};

struct slab_domain {
    /* Actual domain that this corresponds to */
    struct domain *domain;

    /* This domain's slab caches */
    struct slab_cache *local_nonpageable_cache;
    struct slab_cache *local_pageable_cache;

    /* List of slab caches for each distance */
    struct slab_cache_list nonpageable_caches[ALLOC_LOCALITY_MAX + 1];
    struct slab_cache_list pageable_caches[ALLOC_LOCALITY_MAX + 1];

    /* Pointer to an array of pointers to per CPU single-slabs for each class */
    /* # CPUs determined by the domain struct */
    struct slab_per_cpu_cache **per_cpu_caches;

    /* Freequeue for remote frees */
    struct slab_free_queue freequeue;
};

struct slab_page_hdr {
    uint32_t magic;
    uint32_t pages;
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

static inline struct slab_page_hdr *slab_page_hdr_for_addr(void *ptr) {
    return (struct slab_page_hdr *) PAGE_ALIGN_DOWN(ptr);
}

static inline struct slab *slab_for_ptr(void *ptr) {
    return (struct slab *) PAGE_ALIGN_DOWN(ptr);
}

extern struct vas_space *slab_vas;
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
