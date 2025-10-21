#pragma once

#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <misc/list.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/refcount.h>

#define KMALLOC_PAGE_MAGIC 0xC0FFEE42
#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL
#define SLAB_OBJ_ALIGN 16u
#define SLAB_BITMAP_TEST(__bitmap, __idx) (__bitmap & __idx)
#define SLAB_CROSSFREE_RING_SIZE 64
#define SLAB_NONPAGEABLE_RESERVED_RATIO (1 / 16)
#define SLAB_DESTROY_HIGH_WATERMARK 4
#define SLAB_INTERLEAVE_STRIDE 1
#define SLAB_PER_CORE_MAGAZINE_ENTRIES 32
#define SLAB_MIN_SIZE (sizeof(vaddr_t))
#define SLAB_MAX_SIZE (PAGE_SIZE / 4)

static const uint64_t slab_class_sizes[] = {
    SLAB_MIN_SIZE, 16, 32, 64, 96, 128, 192, 256, 512, SLAB_MAX_SIZE};

#define SLAB_CLASS_COUNT (sizeof(slab_class_sizes) / sizeof(*slab_class_sizes))

enum slab_state { SLAB_FREE, SLAB_PARTIAL, SLAB_FULL };

struct slab {
    struct list_head list;

    uint8_t *bitmap;

    vaddr_t mem;
    atomic_uint_fast64_t used;
    struct slab_cache *parent_cache;

    struct spinlock lock;
    refcount_t refcount;
    enum slab_state state;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab, lock);
#define slab_from_list_node(ln) (container_of(ln, struct slab, list))

static inline struct page *slab_get_backing_page(struct slab *slab) {
    return page_for_pfn(PAGE_TO_PFN(vmm_get_phys((vaddr_t) slab)));
}

#define PAGE_NON_SLAB_SPACE (PAGE_SIZE - sizeof(struct slab))

/* Just a simple stack */
struct slab_magazine {
    vaddr_t objs[SLAB_PER_CORE_MAGAZINE_ENTRIES];
    uint8_t count;
};

struct slab_per_cpu_cache {
    struct slab_magazine mag[SLAB_CLASS_COUNT];
    struct slab *active_slab[SLAB_CLASS_COUNT];
};

static inline bool slab_magazine_full(struct slab_magazine *mag) {
    return mag->count == SLAB_PER_CORE_MAGAZINE_ENTRIES;
}

struct slab_free_slot {
    atomic_uint_fast64_t seq;
    vaddr_t addr;
};

/* This is a cheeky little structure that
 * we embed into the data itself when
 * the free_queue ringbuffer is full.
 *
 * The idea is as follows:
 *
 * When we free an address, if the free_queue list
 * is empty, we set the free_queue list's head
 * address to the address that we are freeing,
 * as well as the free_queue list's tail.
 *
 * Upon subsequent frees, we read the
 * free_queue list's tail, and then
 * set its *next to the address we are
 * freeing, and set the tail to point
 * to the new address we are freeing, and
 * repeat this process.
 *
 * Because the minimum slab size is the pointer
 * size, we can guarantee that the pointer fits */

struct slab_free_queue_list {
    struct slab_free_queue_list *next;
};

struct slab_free_queue {
    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;
    size_t capacity;
    struct slab_free_slot *slots;

    struct slab_free_queue_list *list_head;
    struct slab_free_queue_list *list_tail;

    /* For the free_queue_list */
    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT_NAMED(slab_free_queue, lock, list);

enum slab_cache_type { SLAB_CACHE_TYPE_PAGEABLE, SLAB_CACHE_TYPE_NONPAGEABLE };

struct slab_cache {
    uint64_t obj_size;
    uint64_t objs_per_slab;

    struct list_head slabs_free;
    struct list_head slabs_partial;
    struct list_head slabs_full;
    atomic_size_t free_slabs_count;

    enum slab_cache_type type;

    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_cache, lock);

struct slab_caches {
    struct slab_cache caches[SLAB_CLASS_COUNT];
};

struct slab_cache_ref {
    struct slab_domain *domain; /* owning domain of the cache */
    struct slab_caches *caches; /* pointer to caches */
    enum slab_cache_type type;  /* pageable / nonpageable */
    uint8_t locality;           /* NUMA proximity, 0 = local */
};

struct slab_cache_zonelist {
    struct slab_cache_ref *entries;
    size_t count;
};

struct slab_domain {
    /* Actual domain that this corresponds to */
    struct domain *domain;

    /* This domain's slab caches */
    struct slab_caches *local_nonpageable_cache;
    struct slab_caches *local_pageable_cache;

    /* Slab caches for each distance */
    struct slab_cache_zonelist nonpageable_zonelist;
    struct slab_cache_zonelist pageable_zonelist;

    /* Pointer to an array of pointers to per CPU single-slabs for each class */
    /* # CPUs determined by the domain struct */
    struct slab_per_cpu_cache **per_cpu_caches;

    /* Freequeue for remote frees */
    struct slab_free_queue freequeue;

    struct list_head slab_gc_list;
};

struct slab_page_hdr {
    uint32_t magic;
    uint32_t pages;
};

void slab_destroy(struct slab *slab);
int32_t slab_size_to_index(size_t size);
void slab_init();
size_t slab_object_size(vaddr_t addr);

bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj);
vaddr_t slab_magazine_pop(struct slab_magazine *mag);
vaddr_t slab_percpu_alloc(struct slab_domain *dom, size_t class_idx);
void slab_percpu_free(struct slab_domain *dom, size_t class_idx, vaddr_t obj);
void slab_domain_init_percpu(struct slab_domain *dom);
void slab_free_page_hdr(struct slab_page_hdr *hdr);

static inline bool slab_get(struct slab *slab) {
    return refcount_inc(&slab->refcount);
}

static inline void slab_put(struct slab *slab) {
    if (refcount_dec_and_test(&slab->refcount))
        slab_destroy(slab);
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

static inline struct slab_domain *slab_local_domain(void) {
    return smp_core()->slab_domain;
}

static inline struct slab_per_cpu_cache *slab_local_percpu_cache(void) {
    return slab_local_domain()->per_cpu_caches[smp_core_id()];
}

extern struct vas_space *slab_vas;
extern struct slab_cache slab_caches[SLAB_CLASS_COUNT];
